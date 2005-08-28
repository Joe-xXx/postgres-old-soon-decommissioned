/*-------------------------------------------------------------------------
*
* pthread-win32.c
*	 partial pthread implementation for win32
*
* Copyright (c) 2004-2005, PostgreSQL Global Development Group
* IDENTIFICATION
*	$PostgreSQL$
*
*-------------------------------------------------------------------------
*/


#include <windows.h>
#include "pthread-win32.h"

HANDLE
pthread_self()
{
	return GetCurrentThreadId();
}

void
pthread_setspecific(pthread_key_t key, void *val)
{
}

void *
pthread_getspecific(pthread_key_t key)
{
	return NULL;
}

void
pthread_mutex_init(pthread_mutex_t *mp, void *attr)
{
	*mp = CreateMutex(0, 0, 0);
}

void
pthread_mutex_lock(pthread_mutex_t *mp)
{
	WaitForSingleObject(*mp, INFINITE);
}

void
pthread_mutex_unlock(pthread_mutex_t *mp)
{
	ReleaseMutex(*mp);
}
