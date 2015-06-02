/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifdef _WIN32

#include "native_aio_provider.win.h"
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#include <stdio.h>

#define __TITLE__ "aio.provider.native"

namespace dsn { namespace tools {

native_win_aio_provider::native_win_aio_provider(disk_engine* disk, aio_provider* inner_provider)
: aio_provider(disk, inner_provider)
{
    _iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
    _worker_thr = new std::thread(std::bind(&native_win_aio_provider::worker, this));
    ::SetThreadPriority(_worker_thr->native_handle(), THREAD_PRIORITY_HIGHEST);
}

native_win_aio_provider::~native_win_aio_provider()
{
    if (_worker_thr != nullptr && _iocp != NULL && _iocp != INVALID_HANDLE_VALUE)
    {
        ::PostQueuedCompletionStatus(_iocp, 0, 1, NULL);
        
        _worker_thr->join();
        ::CloseHandle(_iocp);
        _iocp = INVALID_HANDLE_VALUE;
        delete _worker_thr;
        _worker_thr = nullptr;
    }
}

handle_t native_win_aio_provider::open(const char* file_name, int oflag, int pmode)
{
	DWORD dwDesiredAccess = 0;
	DWORD dwShareMode = FILE_SHARE_READ;
	DWORD dwCreationDisposition = 0;
	DWORD dwFlagsAndAttributes = FILE_FLAG_OVERLAPPED;

	SECURITY_ATTRIBUTES SecurityAttributes;

	SecurityAttributes.nLength = sizeof(SecurityAttributes);
	SecurityAttributes.lpSecurityDescriptor = NULL;

	if (oflag & _O_NOINHERIT) {
		SecurityAttributes.bInheritHandle = FALSE;
	}
	else {
		SecurityAttributes.bInheritHandle = TRUE;
	}

	/*
	* decode the access flags
	*/
	switch (oflag & (_O_RDONLY | _O_WRONLY | _O_RDWR)) {

	case _O_RDONLY:         /* read access */
		dwDesiredAccess = GENERIC_READ;
		break;
	case _O_WRONLY:         /* write access */
		/* giving it read access as well
		* because in append (a, not a+), we need
		* to read the BOM to determine the encoding
		* (ie. ANSI, UTF8, UTF16)
		*/
		if ((oflag & _O_APPEND)
			&& (oflag & (_O_WTEXT | _O_U16TEXT | _O_U8TEXT)) != 0)
		{
			dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
		}
		else
		{
			dwDesiredAccess = GENERIC_WRITE;
		}
		break;
	case _O_RDWR:           /* read and write access */
		dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
		break;
	default:                /* error, bad oflag */
		_doserrno = 0L; /* not an OS error */
		derror("Invalid open flag\n");
	}

	/*
	* decode open/create method flags
	*/
	switch (oflag & (_O_CREAT | _O_EXCL | _O_TRUNC)) {
	case 0:
	case _O_EXCL:                   // ignore EXCL w/o CREAT
		dwCreationDisposition = OPEN_EXISTING;
		break;

	case _O_CREAT:
		dwCreationDisposition = OPEN_ALWAYS;
		break;

	case _O_CREAT | _O_EXCL:
	case _O_CREAT | _O_TRUNC | _O_EXCL:
		dwCreationDisposition = CREATE_NEW;
		break;

	case _O_TRUNC:
	case _O_TRUNC | _O_EXCL:        // ignore EXCL w/o CREAT
		dwCreationDisposition = TRUNCATE_EXISTING;
		break;

	case _O_CREAT | _O_TRUNC:
		dwCreationDisposition = CREATE_ALWAYS;
		break;

	default:
		// this can't happen ... all cases are covered
		_doserrno = 0L;
		derror("Invalid open flag");
	}

	/*
	* try to open/create the file
	*/
	HANDLE fileHandle = ::CreateFileA(file_name,
		dwDesiredAccess,
		dwShareMode,
		&SecurityAttributes,
		dwCreationDisposition,
		dwFlagsAndAttributes,
		0);

	if (fileHandle != INVALID_HANDLE_VALUE && fileHandle != nullptr)
	{
		if (_iocp != ::CreateIoCompletionPort(fileHandle, _iocp, 0, 0))
		{
			dassert(false, "cannot associate file handle %s to io completion port, err = %x\n", file_name, ::GetLastError());
			return nullptr;
		}
		else
		{
			return fileHandle;
		}
	}
	else
	{
		derror("cannot create file %s, err = %x\n", file_name, ::GetLastError());
		return nullptr;
	}
}

error_code native_win_aio_provider::close(handle_t hFile)
{
    if (::CloseHandle(hFile))
        return ERR_SUCCESS;
    else
    {
        derror("close file failed, err = %x\n", ::GetLastError());
        return ERR_FILE_OPERATION_FAILED;
    }        
}

struct windows_disk_aio_context : public disk_aio
{
    OVERLAPPED olp;
    aio_task*  tsk;
    utils::notify_event*  evt;
    error_code err;
    uint32_t bytes;
};

disk_aio_ptr native_win_aio_provider::prepare_aio_context(aio_task* tsk)
{
    auto r = new windows_disk_aio_context;
    ZeroMemory(&r->olp, sizeof(r->olp));
    r->tsk = tsk;
    r->evt = nullptr;
    return disk_aio_ptr(r);
}

void native_win_aio_provider::aio(aio_task_ptr& aio_tsk)
{
    aio_internal(aio_tsk, true);
}

error_code native_win_aio_provider::aio_internal(aio_task_ptr& aio_tsk, bool async, __out_param uint32_t* pbytes /*= nullptr*/)
{
    auto aio = (windows_disk_aio_context*)aio_tsk->aio().get();
    BOOL r = FALSE;

    aio->olp.Offset = (uint32_t)aio->file_offset;
    aio->olp.OffsetHigh = (uint32_t)(aio->file_offset >> 32);

    if (!async)
    {
        aio->evt = new utils::notify_event();
        aio->err = ERR_SUCCESS;
        aio->bytes = 0;
    }

    switch (aio->type)
    {
    case AIO_Read:
        r = ::ReadFile(aio->file, aio->buffer, aio->buffer_size, NULL, &aio->olp);
        break;
    case AIO_Write:
        r = ::WriteFile(aio->file, aio->buffer, aio->buffer_size, NULL, &aio->olp);
        break;
    default:
        dassert (false, "unknown aio type %u", static_cast<int>(aio->type));
        break;
    }

    if (!r)
    {
        int err = ::GetLastError();
        
        if (err != ERROR_IO_PENDING)
        {
            derror("file operation failed, err = %u", err);

            if (async)
            {
                complete_io(aio_tsk, ERR_FILE_OPERATION_FAILED, 0);
            }
            else
            {
                delete aio->evt;
                aio->evt = nullptr;
            }

            return ERR_FILE_OPERATION_FAILED;
        }
    }

    if (async)
    {
        return ERR_IO_PENDING;
    }
    else
    {
        aio->evt->wait();
        delete aio->evt;
        aio->evt = nullptr;
        *pbytes = aio->bytes;
        return aio->err;
    }
}

void native_win_aio_provider::worker()
{
    DWORD dwTransLen;
    DWORD dwErrorCode;
    ULONG_PTR dwKey;
    LPOVERLAPPED overLap;

    do
    {
        bool ret = (0 != GetQueuedCompletionStatus(_iocp, &dwTransLen, &dwKey, &overLap, INFINITE));

        if (dwKey) break;

        if (ret)
        {
            windows_disk_aio_context* ctx = CONTAINING_RECORD(overLap, windows_disk_aio_context, olp);
            if (!ctx->evt)
            {
                aio_task_ptr aio(ctx->tsk);
                complete_io(aio, ERR_SUCCESS, dwTransLen);
            }
            else
            {
                ctx->err = ERR_SUCCESS;
                ctx->bytes = dwTransLen;
                ctx->evt->notify();
            }
        }
        else if (overLap)
        {
            dwErrorCode = ::GetLastError();
            derror("file operation failed, err = %u", dwErrorCode);

            windows_disk_aio_context* ctx = CONTAINING_RECORD(overLap, windows_disk_aio_context, olp);
            if (!ctx->evt)
            {
                aio_task_ptr aio(ctx->tsk);
                complete_io(aio, ERR_FILE_OPERATION_FAILED, dwTransLen);
            }
            else
            {
                ctx->err = ERR_FILE_OPERATION_FAILED;
                ctx->bytes = dwTransLen;
                ctx->evt->notify();
            }
        }
        else
        {
            ::Sleep(1);
        }
    } while (true);
}

}} // end namespace dsn::tools
#endif
