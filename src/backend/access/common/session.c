/*-------------------------------------------------------------------------
 *
 * session.c
 *		用户会话的封装。
 *
 * 本文件旨在容纳需要在为同一客户端会话工作的各个后端之间进行共享的
 * 数据。特别地，这样的一个会话会在并行查询的领导者（leader）与
 * 工作进程（worker）之间共享。在将来的某个时刻，它也可能成为将后端
 * 与客户端连接分离（例如为了连接池化）的有用基础设施。
 *
 * 目前该基础设施用于共享：
 * - 用于临时（ephemeral）行类型的 typemod 注册表，即 BlessTupleDesc 等。
 *
 * Portions Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * src/backend/access/common/session.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/session.h"
#include "storage/lwlock.h"
#include "storage/shm_toc.h"
#include "utils/memutils.h"
#include "utils/typcache.h"

/* 每个会话 DSM TOC 的魔数（magic number）。 */
#define SESSION_MAGIC						0xabb0fbc9

/*
 * 我们希望创建一个 DSA 区域，用于存储与会话具有相同生命周期的
 * 共享状态。到目前为止，它仅用于保存共享的记录（record）类型注册表。
 * 在常见情况下，我们不希望它此刻就不得不创建任何 DSM 段，因此我们会
 * 为其分配足够的空间来容纳一个非常小的 SharedRecordTypmodRegistry。
 */
#define SESSION_DSA_SIZE					0x30000

/*
 * 用于每个会话 DSM 区域内状态共享的魔数（magic number）。
 */
#define SESSION_KEY_DSA						UINT64CONST(0xFFFFFFFFFFFF0001)
#define SESSION_KEY_RECORD_TYPMOD_REGISTRY	UINT64CONST(0xFFFFFFFFFFFF0002)

/* 本后端当前的会话。 */
Session    *CurrentSession = NULL;

/*
 * 将 CurrentSession 设置为指向一个空的 Session 对象。
 */
void
InitializeSession(void)
{
	CurrentSession = MemoryContextAllocZero(TopMemoryContext, sizeof(Session));
}

/*
 * 如果尚未初始化，则初始化每个会话的 DSM 段，并返回其句柄，以便
 * 工作进程能够附加（attach）到它上面。
 *
 * 与每个上下文的 DSM 段不同，这个段及其内容会被复用，供将来的并行
 * 查询使用。
 *
 * 如果因为缺乏资源而无法分配段，则返回 DSM_HANDLE_INVALID。
 */
dsm_handle
GetSessionDsmHandle(void)
{
	shm_toc_estimator estimator;
	shm_toc    *toc;
	dsm_segment *seg;
	size_t		typmod_registry_size;
	size_t		size;
	void	   *dsa_space;
	void	   *typmod_registry_space;
	dsa_area   *dsa;
	MemoryContext old_context;

	/*
	 * 如果本后端中已经创建过会话作用域的 DSM 段，则返回它的句柄。
	 * 同一个段将在本后端剩余的整个生命周期中被使用。
	 */
	if (CurrentSession->segment != NULL)
		return dsm_segment_handle(CurrentSession->segment);

	/* 否则，准备建立一个段。 */
	old_context = MemoryContextSwitchTo(TopMemoryContext);
	shm_toc_initialize_estimator(&estimator);

	/* 为每个会话的 DSA 区域估算空间。 */
	shm_toc_estimate_keys(&estimator, 1);
	shm_toc_estimate_chunk(&estimator, SESSION_DSA_SIZE);

	/* 为每个会话的记录 typmod 注册表估算空间。 */
	typmod_registry_size = SharedRecordTypmodRegistryEstimate();
	shm_toc_estimate_keys(&estimator, 1);
	shm_toc_estimate_chunk(&estimator, typmod_registry_size);

	/* 建立段与 TOC。 */
	size = shm_toc_estimate(&estimator);
	seg = dsm_create(size, DSM_CREATE_NULL_IF_MAXSEGMENTS);
	if (seg == NULL)
	{
		MemoryContextSwitchTo(old_context);

		return DSM_HANDLE_INVALID;
	}
	toc = shm_toc_create(SESSION_MAGIC,
						 dsm_segment_address(seg),
						 size);

	/* 创建每个会话的 DSA 区域。 */
	dsa_space = shm_toc_allocate(toc, SESSION_DSA_SIZE);
	dsa = dsa_create_in_place(dsa_space,
							  SESSION_DSA_SIZE,
							  LWTRANCHE_PER_SESSION_DSA,
							  seg);
	shm_toc_insert(toc, SESSION_KEY_DSA, dsa_space);


	/* 创建会话作用域的共享记录 typmod 注册表。 */
	typmod_registry_space = shm_toc_allocate(toc, typmod_registry_size);
	SharedRecordTypmodRegistryInit((SharedRecordTypmodRegistry *)
								   typmod_registry_space, seg, dsa);
	shm_toc_insert(toc, SESSION_KEY_RECORD_TYPMOD_REGISTRY,
				   typmod_registry_space);

	/*
	 * 如果我们执行到了这里，就可以钉住（pin）这块共享内存，使其在
	 * 本后端剩余的整个生命周期中都保持映射。如果我们没能走到这一步，
	 * 则为上面已安装的任何东西（即目前是 SharedRecordTypmodRegistry）
	 * 而注册的清理回调，会在 DSM 段被 CurrentResourceOwner 分离（detach）
	 * 时运行，从而不会让我们留下一个损坏的 CurrentSession。
	 */
	dsm_pin_mapping(seg);
	dsa_pin_mapping(dsa);

	/* 通过 CurrentSession 使段与区域可用。 */
	CurrentSession->segment = seg;
	CurrentSession->area = dsa;

	MemoryContextSwitchTo(old_context);

	return dsm_segment_handle(seg);
}

/*
 * 附加（attach）到由并行查询领导者（parallel leader）提供的每个会话
 * DSM 段。
 */
void
AttachSession(dsm_handle handle)
{
	dsm_segment *seg;
	shm_toc    *toc;
	void	   *dsa_space;
	void	   *typmod_registry_space;
	dsa_area   *dsa;
	MemoryContext old_context;

	old_context = MemoryContextSwitchTo(TopMemoryContext);

	/* 附加到 DSM 段。 */
	seg = dsm_attach(handle);
	if (seg == NULL)
		elog(ERROR, "could not attach to per-session DSM segment");
	toc = shm_toc_attach(SESSION_MAGIC, dsm_segment_address(seg));

	/* 附加到 DSA 区域。 */
	dsa_space = shm_toc_lookup(toc, SESSION_KEY_DSA, false);
	dsa = dsa_attach_in_place(dsa_space, seg);

	/* 通过当前会话使它们可用。 */
	CurrentSession->segment = seg;
	CurrentSession->area = dsa;

	/* 附加到共享的记录 typmod 注册表。 */
	typmod_registry_space =
		shm_toc_lookup(toc, SESSION_KEY_RECORD_TYPMOD_REGISTRY, false);
	SharedRecordTypmodRegistryAttach((SharedRecordTypmodRegistry *)
									 typmod_registry_space);

	/* 保持附加状态，直到后端结束或调用 DetachSession()。 */
	dsm_pin_mapping(seg);
	dsa_pin_mapping(dsa);

	MemoryContextSwitchTo(old_context);
}

/*
 * 从当前会话的 DSM 段分离（detach）。显式地这样做并非绝对必要，因为
 * 我们在后端退出时会自动分离；但如果将来会复用并行工作进程，那么
 * 工作进程在附加到另一个会话之前先从当前会话分离就显得很重要了。
 * 注意，本函数会运行分离（detach）钩子。
 */
void
DetachSession(void)
{
	/* 运行分离（detach）钩子。 */
	dsm_detach(CurrentSession->segment);
	CurrentSession->segment = NULL;
	dsa_detach(CurrentSession->area);
	CurrentSession->area = NULL;
}
