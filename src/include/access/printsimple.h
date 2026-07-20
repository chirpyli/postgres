/*-------------------------------------------------------------------------
 *
 * printsimple.h
 *	  打印简单元组，无需访问系统表
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/printsimple.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PRINTSIMPLE_H
#define PRINTSIMPLE_H

#include "tcop/dest.h"

extern bool printsimple(TupleTableSlot *slot, DestReceiver *self);
extern void printsimple_startup(DestReceiver *self, int operation,
								TupleDesc tupdesc);

#endif							/* PRINTSIMPLE_H */
