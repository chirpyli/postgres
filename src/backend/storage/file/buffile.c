/*-------------------------------------------------------------------------
 *
 * buffile.c
 *	  大型缓冲临时文件的管理。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/buffile.c
 *
 * NOTES:
 *
 * BufFile 在虚拟文件（由 fd.c 管理）之上提供了一个非常不完整的 stdio
 * 模拟。目前，我们仅支持 stdio 的缓冲 I/O 层面：只有在缓冲区填满或
 * 清空时才执行底层 File 的读写。对于虚拟文件而言，这比普通内核文件
 * 的收益更大，因为减少虚拟文件的访问频率可以减轻文件描述符的打开/
 * 关闭"抖动"。
 *
 * 注意，BufFile 结构体是用 palloc() 分配的，因此会在查询/事务结束时
 * 自动销毁。由于底层虚拟文件使用 OpenTemporaryFile 创建，即使处理过程
 * 被 ereport(ERROR) 中止，文件的所有资源也一定会被清理。所需的数据
 * 结构在 BufFile 创建时所在的 palloc 上下文中分配，外部资源（如临时
 * 文件）归当时当前的 ResourceOwner 所有。
 *
 * BufFile 还支持超过操作系统文件大小限制的临时文件（通过打开多个 fd.c
 * 临时文件实现）。对于大量数据的排序和哈希连接，这是一个基本特性。
 *
 * BufFile 支持可与其他后端共享的临时文件，作为并行执行的基础设施。
 * 此类文件需要作为所有参与者都附加到的 SharedFileSet 的成员来创建。
 *
 * BufFile 还支持由单个后端使用的临时文件，当对应文件需要在事务间持续
 * 存在且需要多次打开和关闭时使用。此类文件需要作为 FileSet 的成员来
 * 创建。
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "utils/resowner.h"

/*
 * 我们将 BufFile 按 GB 大小分片，与 RELSEG_SIZE 无关。
 * 原因是希望大 BufFile 在可用时能分散到多个表空间中。
 */
#define MAX_PHYSICAL_FILESIZE	0x40000000
#define BUFFILE_SEG_SIZE		(MAX_PHYSICAL_FILESIZE / BLCKSZ)

/*
 * 此数据结构表示一个由一个或多个物理文件（每个通过 fd.c 管理的
 * 虚拟文件描述符访问）组成的缓冲文件。
 */
struct BufFile
{
	int			numFiles;		/* 集合中物理文件的数量 */
	/* 除最后一个文件外，所有文件长度恰好为 MAX_PHYSICAL_FILESIZE */
	File	   *files;			/* 通过 palloc 分配、包含 numFiles 个条目的数组 */

	bool		isInterXact;	/* 是否跨事务保持打开？ */
	bool		dirty;			/* 缓冲区是否需要写出？ */
	bool		readOnly;		/* 文件是否已设为只读？ */

	FileSet    *fileset;		/* 基于 fileset 的分片文件空间 */
	const char *name;			/* 基于 fileset 的 BufFile 的名称 */

	/*
	 * resowner 是底层临时文件使用的 ResourceOwner。（我们不需要显式记录
	 * 使用的内存上下文，因为创建后我们只会 repalloc 扩大数组。）
	 */
	ResourceOwner resowner;

	/*
	 * "当前位置" 是缓冲区在逻辑文件中的起始位置。
	 * BufFile 用户看到的位置是 (curFile, curOffset + pos)。
	 */
	int			curFile;		/* 当前位置的文件索引（0..n） */
	off_t		curOffset;		/* 当前位置的偏移量 */
	int			pos;			/* 缓冲区中下一个读/写位置 */
	int			nbytes;			/* 缓冲区中有效字节总数 */

	/*
	 * XXX 理想情况下应使用 PGIOAlignedBlock，但可能需要一种方法避免
	 * 在用户创建许多文件时浪费每个文件的对齐填充。
	 */
	PGAlignedBlock buffer;
};

static BufFile *makeBufFileCommon(int nfiles);
static BufFile *makeBufFile(File firstfile);
static void extendBufFile(BufFile *file);
static void BufFileLoadBuffer(BufFile *file);
static void BufFileDumpBuffer(BufFile *file);
static void BufFileFlush(BufFile *file);
static File MakeNewFileSetSegment(BufFile *buffile, int segment);

/*
 * 创建 BufFile 并执行公共初始化。
 */
static BufFile *
makeBufFileCommon(int nfiles)
{
	BufFile    *file = (BufFile *) palloc(sizeof(BufFile));

	file->numFiles = nfiles;
	file->isInterXact = false;
	file->dirty = false;
	file->resowner = CurrentResourceOwner;
	file->curFile = 0;
	file->curOffset = 0;
	file->pos = 0;
	file->nbytes = 0;

	return file;
}

/*
 * 给定第一个底层物理文件，创建一个 BufFile。
 * 注意：调用者必须根据情况设置 isInterXact。
 */
static BufFile *
makeBufFile(File firstfile)
{
	BufFile    *file = makeBufFileCommon(1);

	file->files = (File *) palloc(sizeof(File));
	file->files[0] = firstfile;
	file->readOnly = false;
	file->fileset = NULL;
	file->name = NULL;

	return file;
}

/*
 * 添加另一个组件临时文件。
 */
static void
extendBufFile(BufFile *file)
{
	File		pfile;
	ResourceOwner oldowner;

	/* 确保文件与 BufFile 的 resource owner 关联 */
	oldowner = CurrentResourceOwner;
	CurrentResourceOwner = file->resowner;

	if (file->fileset == NULL)
		pfile = OpenTemporaryFile(file->isInterXact);
	else
		pfile = MakeNewFileSetSegment(file, file->numFiles);

	Assert(pfile >= 0);

	CurrentResourceOwner = oldowner;

	file->files = (File *) repalloc(file->files,
									(file->numFiles + 1) * sizeof(File));
	file->files[file->numFiles] = pfile;
	file->numFiles++;
}

/*
 * 为新临时文件创建一个 BufFile（如果写入超过 MAX_PHYSICAL_FILESIZE
 * 字节，将扩展为多个临时文件）。
 *
 * 如果 interXact 为 true，临时文件不会在事务结束时自动删除。
 *
 * 注意：如果 interXact 为 true，调用者最好在能跨越事务边界存活
 * 的内存上下文和 resource owner 中调用我们。
 */
BufFile *
BufFileCreateTemp(bool interXact)
{
	BufFile    *file;
	File		pfile;

	/*
	 * 确保临时表空间已设置好供 OpenTemporaryFile 使用。
	 * 调用者可能已完成此操作，但在此处再次检查是有益的。如果完全
	 * 不做此操作，将导致临时文件始终放置在默认表空间中，这是一个
	 * 难以发现的 bug。调用者如果希望确保任何必需的 catalog 访问在
	 * 其他资源上下文中完成，可能倾向于提前调用。
	 */
	PrepareTempTablespaces();

	pfile = OpenTemporaryFile(interXact);
	Assert(pfile >= 0);

	file = makeBufFile(pfile);
	file->isInterXact = interXact;

	return file;
}

/*
 * 为给定 BufFile 的指定分片构建名称。
 */
static void
FileSetSegmentName(char *name, const char *buffile_name, int segment)
{
	snprintf(name, MAXPGPATH, "%s.%d", buffile_name, segment);
}

/*
 * 创建支持基于 fileset 的 BufFile 的新分片文件。
 */
static File
MakeNewFileSetSegment(BufFile *buffile, int segment)
{
	char		name[MAXPGPATH];
	File		file;

	/*
	 * 可能存在崩溃重启前遗留的同名文件。为了不让 BufFileOpenFileSet()
	 * 对分片数量产生混淆，如果下一个分片号已存在，我们先将其删除。
	 */
	FileSetSegmentName(name, buffile->name, segment + 1);
	FileSetDelete(buffile->fileset, name, true);

	/* 创建新分片。 */
	FileSetSegmentName(name, buffile->name, segment);
	file = FileSetCreate(buffile->fileset, name);

	/* FileSetCreate 会在错误时通过 ereport 报错 */
	Assert(file > 0);

	return file;
}

/*
 * 创建一个可通过相同名称被附加到同一 SharedFileSet 的其他后端发现
 * 并以只读方式打开的 BufFile。
 *
 * 基于 fileset 的 BufFile 的命名方案由调用代码决定。名称将作为磁盘上
 * 一个或多个文件名的一部分出现，可能为管理员提供关于哪个子系统正在
 * 生成临时文件数据的线索。由于每个 SharedFileSet 对象由一个或多个
 * 唯一命名的临时目录支持，名称不会与无关的 SharedFileSet 对象冲突。
 */
BufFile *
BufFileCreateFileSet(FileSet *fileset, const char *name)
{
	BufFile    *file;

	file = makeBufFileCommon(1);
	file->fileset = fileset;
	file->name = pstrdup(name);
	file->files = (File *) palloc(sizeof(File));
	file->files[0] = MakeNewFileSetSegment(file, 0);
	file->readOnly = false;

	return file;
}

/*
 * 打开先前在另一个后端（或本后端）中使用 BufFileCreateFileSet 在同一
 * FileSet 中以相同名称创建的文件。创建该文件的后端必须已调用
 * BufFileClose() 或 BufFileExportFileSet()，以确保它已准备好被其他
 * 后端打开，并将其设为只读。如果 missing_ok 为 true，表示缺失文件
 * 可以安全忽略，则当未找到给定名称的 BufFile 时返回 NULL，
 * 否则抛出错误。
 */
BufFile *
BufFileOpenFileSet(FileSet *fileset, const char *name, int mode,
				   bool missing_ok)
{
	BufFile    *file;
	char		segment_name[MAXPGPATH];
	Size		capacity = 16;
	File	   *files;
	int			nfiles = 0;

	files = palloc(sizeof(File) * capacity);

	/*
	 * 我们不知道有多少分片，因此将探测文件系统来找出答案。
	 */
	for (;;)
	{
		/* 检查是否需要扩展文件分片数组。 */
		if (nfiles + 1 > capacity)
		{
			capacity *= 2;
			files = repalloc(files, sizeof(File) * capacity);
		}
		/* 尝试加载一个分片。 */
		FileSetSegmentName(segment_name, name, nfiles);
		files[nfiles] = FileSetOpen(fileset, segment_name, mode);
		if (files[nfiles] <= 0)
			break;
		++nfiles;

		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * 如果完全没有找到任何文件，则不存在此名称的 BufFile。
	 */
	if (nfiles == 0)
	{
		/* 释放内存 */
		pfree(files);

		if (missing_ok)
			return NULL;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open temporary file \"%s\" from BufFile \"%s\": %m",
						segment_name, name)));
	}

	file = makeBufFileCommon(nfiles);
	file->files = files;
	file->readOnly = (mode == O_RDONLY);
	file->fileset = fileset;
	file->name = pstrdup(name);

	return file;
}

/*
 * 删除在给定 FileSet 中使用给定名称通过 BufFileCreateFileSet 创建的
 * BufFile。
 *
 * 不必使用此函数显式删除文件。它仅作为主动删除文件的一种方式提供，
 * 而不是等待 FileSet 被清理。
 *
 * 只能有一个后端尝试删除给定名称的文件，并且应知道该文件已存在并已
 * 导出或关闭，否则应将 missing_ok 设为 true。
 */
void
BufFileDeleteFileSet(FileSet *fileset, const char *name, bool missing_ok)
{
	char		segment_name[MAXPGPATH];
	int			segment = 0;
	bool		found = false;

	/*
	 * 我们不知道文件有多少分片。将持续删除直到删完。如果连初始分片
	 * 都找不到，则引发错误。
	 */
	for (;;)
	{
		FileSetSegmentName(segment_name, name, segment);
		if (!FileSetDelete(fileset, segment_name, true))
			break;
		found = true;
		++segment;

		CHECK_FOR_INTERRUPTS();
	}

	if (!found && !missing_ok)
		elog(ERROR, "could not delete unknown BufFile \"%s\"", name);
}

/*
 * BufFileExportFileSet --- 刷新并设为只读，为共享做准备。
 */
void
BufFileExportFileSet(BufFile *file)
{
	/* 必须是属于 FileSet 的文件。 */
	Assert(file->fileset != NULL);

	/* 如果调用两次，很可能是一个 bug。 */
	Assert(!file->readOnly);

	BufFileFlush(file);
	file->readOnly = true;
}

/*
 * 关闭一个 BufFile
 *
 * 类似 fclose()，这也隐式地对底层 File 执行 FileClose。
 */
void
BufFileClose(BufFile *file)
{
	int			i;

	/* 刷新所有未写数据 */
	BufFileFlush(file);
	/* 关闭并删除底层文件 */
	for (i = 0; i < file->numFiles; i++)
		FileClose(file->files[i]);
	/* 释放缓冲区空间 */
	pfree(file->files);
	pfree(file);
}

/*
 * BufFileLoadBuffer
 *
 * 如果可能，从 curOffset 开始将一些数据加载到缓冲区。
 * 调用时，必须满足 dirty = false、pos 和 nbytes = 0。
 * 退出时，nbytes 是已加载的字节数。
 */
static void
BufFileLoadBuffer(BufFile *file)
{
	File		thisfile;
	instr_time	io_start;
	instr_time	io_time;

	/*
	 * 如有必要且可能，前进到下一个组件文件。
	 */
	if (file->curOffset >= MAX_PHYSICAL_FILESIZE &&
		file->curFile + 1 < file->numFiles)
	{
		file->curFile++;
		file->curOffset = 0;
	}

	thisfile = file->files[file->curFile];

	if (track_io_timing)
		INSTR_TIME_SET_CURRENT(io_start);
	else
		INSTR_TIME_SET_ZERO(io_start);

	/*
	 * 尽可能多地读取，最多一整个缓冲区大小。
	 */
	file->nbytes = FileRead(thisfile,
							file->buffer.data,
							sizeof(file->buffer.data),
							file->curOffset,
							WAIT_EVENT_BUFFILE_READ);
	if (file->nbytes < 0)
	{
		file->nbytes = 0;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						FilePathName(thisfile))));
	}

	if (track_io_timing)
	{
		INSTR_TIME_SET_CURRENT(io_time);
		INSTR_TIME_ACCUM_DIFF(pgBufferUsage.temp_blk_read_time, io_time, io_start);
	}

	/* 这里选择不推进 curOffset */

	if (file->nbytes > 0)
		pgBufferUsage.temp_blks_read++;
}

/*
 * BufFileDumpBuffer
 *
 * 从 curOffset 开始将缓冲区内容写出。
 * 调用时，应有 dirty = true, nbytes > 0。
 * 退出时，若写入成功则清除 dirty，并推进 curOffset。
 */
static void
BufFileDumpBuffer(BufFile *file)
{
	int			wpos = 0;
	int			bytestowrite;
	File		thisfile;

	/*
	 * 与 BufFileLoadBuffer 不同，即使跨越组件文件边界，我们也必须
	 * 写出整个缓冲区；因此需要循环。
	 */
	while (wpos < file->nbytes)
	{
		off_t		availbytes;
		instr_time	io_start;
		instr_time	io_time;

		/*
		 * 如有必要且可能，前进到下一个组件文件。
		 */
		if (file->curOffset >= MAX_PHYSICAL_FILESIZE)
		{
			while (file->curFile + 1 >= file->numFiles)
				extendBufFile(file);
			file->curFile++;
			file->curOffset = 0;
		}

		/*
		 * 确定需要向此文件中写入多少数据。
		 */
		bytestowrite = file->nbytes - wpos;
		availbytes = MAX_PHYSICAL_FILESIZE - file->curOffset;

		if ((off_t) bytestowrite > availbytes)
			bytestowrite = (int) availbytes;

		thisfile = file->files[file->curFile];

		if (track_io_timing)
			INSTR_TIME_SET_CURRENT(io_start);
		else
			INSTR_TIME_SET_ZERO(io_start);

		bytestowrite = FileWrite(thisfile,
								 file->buffer.data + wpos,
								 bytestowrite,
								 file->curOffset,
								 WAIT_EVENT_BUFFILE_WRITE);
		if (bytestowrite <= 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m",
							FilePathName(thisfile))));

		if (track_io_timing)
		{
			INSTR_TIME_SET_CURRENT(io_time);
			INSTR_TIME_ACCUM_DIFF(pgBufferUsage.temp_blk_write_time, io_time, io_start);
		}

		file->curOffset += bytestowrite;
		wpos += bytestowrite;

		pgBufferUsage.temp_blks_written++;
	}
	file->dirty = false;

	/*
	 * 此时 curOffset 已推进到缓冲区末尾，即 原始值 + nbytes。
	 * 我们需要将其指向逻辑文件位置，即 原始值 + pos，以防它更小
	 * （可能由于脏缓冲区中的小步后退 seek 而发生！）。
	 */
	file->curOffset -= (file->nbytes - file->pos);
	if (file->curOffset < 0)	/* 处理可能的跨分片情况 */
	{
		file->curFile--;
		Assert(file->curFile >= 0);
		file->curOffset += MAX_PHYSICAL_FILESIZE;
	}

	/*
	 * 现在可以将缓冲区清空而不改变逻辑位置
	 */
	file->pos = 0;
	file->nbytes = 0;
}

/*
 * BufFileRead 变体
 *
 * 类似 fread()，但假定元素大小为 1 字节，并通过 ereport() 报告 I/O 错误。
 *
 * 如果 'exact' 为 true，则当读取的字节数不完全等于 'size' 时也会引发错误
 * （不允许短读）。如果 'exact' 和 'eofOK' 均为 true，则读取零字节是允许的。
 */
static size_t
BufFileReadCommon(BufFile *file, void *ptr, size_t size, bool exact, bool eofOK)
{
	size_t		start_size = size;
	size_t		nread = 0;
	size_t		nthistime;

	BufFileFlush(file);

	while (size > 0)
	{
		if (file->pos >= file->nbytes)
		{
			/* 尝试将更多数据加载到缓冲区。 */
			file->curOffset += file->pos;
			file->pos = 0;
			file->nbytes = 0;
			BufFileLoadBuffer(file);
			if (file->nbytes <= 0)
				break;			/* 没有更多数据可用 */
		}

		nthistime = file->nbytes - file->pos;
		if (nthistime > size)
			nthistime = size;
		Assert(nthistime > 0);

		memcpy(ptr, file->buffer.data + file->pos, nthistime);

		file->pos += nthistime;
		ptr = (char *) ptr + nthistime;
		size -= nthistime;
		nread += nthistime;
	}

	if (exact &&
		(nread != start_size && !(nread == 0 && eofOK)))
		ereport(ERROR,
				errcode_for_file_access(),
				file->name ?
				errmsg("could not read from file set \"%s\": read only %zu of %zu bytes",
					   file->name, nread, start_size) :
				errmsg("could not read from temporary file: read only %zu of %zu bytes",
					   nread, start_size));

	return nread;
}

/*
 * 传统接口，调用者需要自行检查文件末尾或短读。
 */
size_t
BufFileRead(BufFile *file, void *ptr, size_t size)
{
	return BufFileReadCommon(file, ptr, size, false, false);
}

/*
 * 要求正好读取指定大小的数据。
 */
void
BufFileReadExact(BufFile *file, void *ptr, size_t size)
{
	BufFileReadCommon(file, ptr, size, true, false);
}

/*
 * 要求正好读取指定大小的数据，但可选择允许文件末尾（此时返回 0）。
 */
size_t
BufFileReadMaybeEOF(BufFile *file, void *ptr, size_t size, bool eofOK)
{
	return BufFileReadCommon(file, ptr, size, true, eofOK);
}

/*
 * BufFileWrite
 *
 * 类似 fwrite()，但假定元素大小为 1 字节，并通过 ereport() 报告错误。
 */
void
BufFileWrite(BufFile *file, const void *ptr, size_t size)
{
	size_t		nthistime;

	Assert(!file->readOnly);

	while (size > 0)
	{
		if (file->pos >= BLCKSZ)
		{
			/* 缓冲区已满，将其写出 */
			if (file->dirty)
				BufFileDumpBuffer(file);
			else
			{
				/* 嗯，直接从读取切换到写入了？ */
				file->curOffset += file->pos;
				file->pos = 0;
				file->nbytes = 0;
			}
		}

		nthistime = BLCKSZ - file->pos;
		if (nthistime > size)
			nthistime = size;
		Assert(nthistime > 0);

		memcpy(file->buffer.data + file->pos, ptr, nthistime);

		file->dirty = true;
		file->pos += nthistime;
		if (file->nbytes < file->pos)
			file->nbytes = file->pos;
		ptr = (const char *) ptr + nthistime;
		size -= nthistime;
	}
}

/*
 * BufFileFlush
 *
 * 类似 fflush()，只是 I/O 错误通过 ereport() 报告。
 */
static void
BufFileFlush(BufFile *file)
{
	if (file->dirty)
		BufFileDumpBuffer(file);

	Assert(!file->dirty);
}

/*
 * BufFileSeek
 *
 * 类似 fseek()，但目标位置需要两个值，以便在逻辑文件大小超过 off_t
 * 可表示的最大值时仍能工作。不过，我们不支持跨越大于该范围的相对 seek。
 * I/O 错误通过 ereport() 报告。
 *
 * 成功返回 0，失败返回 EOF。如果尝试不可能的 seek，逻辑位置不会被移动。
 */
int
BufFileSeek(BufFile *file, int fileno, off_t offset, int whence)
{
	int			newFile;
	off_t		newOffset;

	switch (whence)
	{
		case SEEK_SET:
			if (fileno < 0)
				return EOF;
			newFile = fileno;
			newOffset = offset;
			break;
		case SEEK_CUR:

			/*
			 * 相对 seek 只考虑有符号偏移量，忽略 fileno。
			 * 注意大偏移量（> 1 GB）在此加法中有溢出风险，除非使用 64 位 off_t。
			 */
			newFile = file->curFile;
			newOffset = (file->curOffset + file->pos) + offset;
			break;
		case SEEK_END:

			/*
			 * 最后一个文件的大小给出了该文件的结束偏移。
			 */
			newFile = file->numFiles - 1;
			newOffset = FileSize(file->files[file->numFiles - 1]);
			if (newOffset < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not determine size of temporary file \"%s\" from BufFile \"%s\": %m",
								FilePathName(file->files[file->numFiles - 1]),
								file->name)));
			break;
		default:
			elog(ERROR, "invalid whence: %d", whence);
			return EOF;
	}
	while (newOffset < 0)
	{
		if (--newFile < 0)
			return EOF;
		newOffset += MAX_PHYSICAL_FILESIZE;
	}
	if (newFile == file->curFile &&
		newOffset >= file->curOffset &&
		newOffset <= file->curOffset + file->nbytes)
	{
		/*
		 * seek 位置在现有缓冲区范围内；只需调整缓冲区内的位置即可，
		 * 无需刷新缓冲区。注意，无论是读还是写都可以这样做，但如果是
		 * 写操作，缓冲区仍保持脏状态。
		 */
		file->pos = (int) (newOffset - file->curOffset);
		return 0;
	}
	/* 否则必须重定位缓冲区，因此先刷新所有脏数据 */
	BufFileFlush(file);

	/*
	 * 在此处（不能更早）检查是否 seek 到了最后一个分片之后。
	 * 上述 flush 可能创建了新分片，所以更早检查会不起作用
	 * （至少在当前的代码逻辑下）。
	 */

	/* 将 "下一分片开头" 的 seek 转换为 "上一分片末尾" */
	if (newFile == file->numFiles && newOffset == 0)
	{
		newFile--;
		newOffset = MAX_PHYSICAL_FILESIZE;
	}
	while (newOffset > MAX_PHYSICAL_FILESIZE)
	{
		if (++newFile >= file->numFiles)
			return EOF;
		newOffset -= MAX_PHYSICAL_FILESIZE;
	}
	if (newFile >= file->numFiles)
		return EOF;
	/* Seek 成功！ */
	file->curFile = newFile;
	file->curOffset = newOffset;
	file->pos = 0;
	file->nbytes = 0;
	return 0;
}

void
BufFileTell(BufFile *file, int *fileno, off_t *offset)
{
	*fileno = file->curFile;
	*offset = file->curOffset + file->pos;
}

/*
 * BufFileSeekBlock --- 面向块的 seek
 *
 * 执行绝对 seek 到文件中第 n 个 BLCKSZ 大小块的开头。
 * 注意，如果文件超过 BLCKSZ * PG_INT64_MAX 字节，此接口的用户会失败，
 * 但这是相当大的量；我们处理的表也不会超过这个大小……
 *
 * 成功返回 0，失败返回 EOF。如果尝试不可能的 seek，逻辑位置不会被移动。
 */
int
BufFileSeekBlock(BufFile *file, int64 blknum)
{
	return BufFileSeek(file,
					   (int) (blknum / BUFFILE_SEG_SIZE),
					   (off_t) (blknum % BUFFILE_SEG_SIZE) * BLCKSZ,
					   SEEK_SET);
}

/*
 * 返回给定 BufFile 中的数据量，以字节为单位。
 *
 * 返回值包括 BufFileAppend 留下的空洞大小。
 * 失败时通过 ereport() 报错。
 */
int64
BufFileSize(BufFile *file)
{
	int64		lastFileSize;

	/* 获取最后一个物理文件的大小。 */
	lastFileSize = FileSize(file->files[file->numFiles - 1]);
	if (lastFileSize < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not determine size of temporary file \"%s\" from BufFile \"%s\": %m",
						FilePathName(file->files[file->numFiles - 1]),
						file->name)));

	return ((file->numFiles - 1) * (int64) MAX_PHYSICAL_FILESIZE) +
		lastFileSize;
}

/*
 * 将源文件的内容追加到目标文件末尾。
 *
 * 注意，此操作会接管 "source" 底层资源的所有权。调用者在调用此函数后
 * 绝不应再对 source 调用 BufFileClose。source 和 target 的 resource
 * owner 也必须匹配。
 *
 * 此操作通过操作分片文件列表来工作，因此文件内容总是在
 * MAX_PHYSICAL_FILESIZE 对齐的边界处追加，通常会在边界前产生空空洞。
 * 这些区域不包含任何有意义的数据，调用者无法从中读取。
 *
 * 返回源文件内容在目标文件中开始的块号。调用者在使用基于原始 BufFile
 * 空间的块位置时，应将其作为偏移量应用。
 */
int64
BufFileAppend(BufFile *target, BufFile *source)
{
	int64		startBlock = (int64) target->numFiles * BUFFILE_SEG_SIZE;
	int			newNumFiles = target->numFiles + source->numFiles;
	int			i;

	Assert(source->readOnly);
	Assert(!source->dirty);

	if (target->resowner != source->resowner)
		elog(ERROR, "could not append BufFile with non-matching resource owner");

	target->files = (File *)
		repalloc(target->files, sizeof(File) * newNumFiles);
	for (i = target->numFiles; i < newNumFiles; i++)
		target->files[i] = source->files[i - target->numFiles];
	target->numFiles = newNumFiles;

	return startBlock;
}

/*
 * 将通过 BufFileCreateFileSet 创建的 BufFile 截断到给定的 fileno 和偏移量。
 */
void
BufFileTruncateFileSet(BufFile *file, int fileno, off_t offset)
{
	int			numFiles = file->numFiles;
	int			newFile = fileno;
	off_t		newOffset = file->curOffset;
	char		segment_name[MAXPGPATH];
	int			i;

	/*
	 * 遍历从当前文件到给定 fileno 的所有文件，删除大于 fileno 的文件，
	 * 并将给定文件截断到指定的偏移量。注意，如果偏移量为 0，我们也会
	 * 删除给定的 fileno，前提是它不是第一个文件（第一个文件我们会截断它）。
	 */
	for (i = file->numFiles - 1; i >= fileno; i--)
	{
		if ((i != fileno || offset == 0) && i != 0)
		{
			FileSetSegmentName(segment_name, file->name, i);
			FileClose(file->files[i]);
			if (!FileSetDelete(file->fileset, segment_name, true))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not delete fileset \"%s\": %m",
								segment_name)));
			numFiles--;
			newOffset = MAX_PHYSICAL_FILESIZE;

			/*
			 * 以此标记我们已经删除了给定的 fileno。
			 */
			if (i == fileno)
				newFile--;
		}
		else
		{
			if (FileTruncate(file->files[i], offset,
							 WAIT_EVENT_BUFFILE_TRUNCATE) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not truncate file \"%s\": %m",
								FilePathName(file->files[i]))));
			newOffset = offset;
		}
	}

	file->numFiles = numFiles;

	/*
	 * 如果截断点在现有缓冲区范围内，则只需调整缓冲区内的位置。
	 */
	if (newFile == file->curFile &&
		newOffset >= file->curOffset &&
		newOffset <= file->curOffset + file->nbytes)
	{
		/* 如果新位置更靠后，无需重置当前位置。 */
		if (newOffset <= file->curOffset + file->pos)
			file->pos = (int) (newOffset - file->curOffset);

		/* 调整当前缓冲区的 nbytes。 */
		file->nbytes = (int) (newOffset - file->curOffset);
	}
	else if (newFile == file->curFile &&
			 newOffset < file->curOffset)
	{
		/*
		 * 截断点在当前文件内但位于当前位置之前，因此可以丢弃当前缓冲区
		 * 并重置当前位置。
		 */
		file->curOffset = newOffset;
		file->pos = 0;
		file->nbytes = 0;
	}
	else if (newFile < file->curFile)
	{
		/*
		 * 截断点在当前文件之前，因此需要相应地重置当前位置。
		 */
		file->curFile = newFile;
		file->curOffset = newOffset;
		file->pos = 0;
		file->nbytes = 0;
	}
	/* 如果截断点在当前文件之后，则无需执行任何操作。 */
}
