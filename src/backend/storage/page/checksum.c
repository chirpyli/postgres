/*-------------------------------------------------------------------------
 *
 * checksum.c
 *	  Checksum implementation for data pages.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/page/checksum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/checksum.h"
/*
 * 实际的代码在 storage/checksum_impl.h 中。这样做是为了让
 * 外部程序可以通过 #include 该文件（从导出的 Postgres 头文件中）
 * 来复用这段校验和代码。（可对比我们的 CRC 代码。）
 */
#include "storage/checksum_impl.h"	/* IWYU pragma: keep */
