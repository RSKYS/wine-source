/* Unit test suite for SHLWAPI ShCreateStreamOnFile functions.
 *
 * Copyright 2008 Reece H. Dunn
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <stdarg.h>
#include <stdio.h>

#include "wine/test.h"
#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "shlwapi.h"


/* Function pointers for the SHCreateStreamOnFile functions */
static HMODULE hShlwapi;
static HRESULT (WINAPI *pSHCreateStreamOnFileA)(LPCSTR file, DWORD mode, IStream **stream);
static HRESULT (WINAPI *pSHCreateStreamOnFileW)(LPCWSTR file, DWORD mode, IStream **stream);
static HRESULT (WINAPI *pSHCreateStreamOnFileEx)(LPCWSTR file, DWORD mode, DWORD attributes, BOOL create, IStream *template, IStream **stream);


static void test_IStream_invalid_operations(IStream * stream, DWORD mode)
{
    HRESULT ret;
    IStream * clone;
    ULONG refcount;
    ULARGE_INTEGER uzero;
    ULARGE_INTEGER uret;
    LARGE_INTEGER zero;
    ULONG count;
    char data[256];

    U(uzero).HighPart = 0;
    U(uzero).LowPart = 0;
    U(uret).HighPart = 0;
    U(uret).LowPart = 0;
    U(zero).HighPart = 0;
    U(zero).LowPart = 0;

    /* IStream::Read */

    /* IStream_Read from the COBJMACROS is undefined by shlwapi.h, replaced by the IStream_Read helper function. */

    ret = stream->lpVtbl->Read(stream, NULL, 0, &count);
    ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    ret = stream->lpVtbl->Read(stream, data, 5, NULL);
    ok(ret == S_FALSE || ret == S_OK, "expected S_FALSE or S_OK, got 0x%08x\n", ret);

    ret = stream->lpVtbl->Read(stream, data, 0, NULL);
    ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    ret = stream->lpVtbl->Read(stream, data, 3, &count);
    ok(ret == S_FALSE || ret == S_OK, "expected S_FALSE or S_OK, got 0x%08x\n", ret);

    /* IStream::Write */

    /* IStream_Write from the COBJMACROS is undefined by shlwapi.h, replaced by the IStream_Write helper function. */

    ret = stream->lpVtbl->Write(stream, NULL, 0, &count);
    if (mode == STGM_READ)
        ok(ret == STG_E_ACCESSDENIED, "expected STG_E_ACCESSDENIED, got 0x%08x\n", ret);
    else
        ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    strcpy(data, "Hello");
    ret = stream->lpVtbl->Write(stream, data, 5, NULL);
    if (mode == STGM_READ)
        ok(ret == STG_E_ACCESSDENIED, "expected STG_E_ACCESSDENIED, got 0x%08x\n", ret);
    else
        ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    strcpy(data, "Hello");
    ret = stream->lpVtbl->Write(stream, data, 0, NULL);
    if (mode == STGM_READ)
        ok(ret == STG_E_ACCESSDENIED, "expected STG_E_ACCESSDENIED, got 0x%08x\n", ret);
    else
        ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    strcpy(data, "Hello");
    ret = stream->lpVtbl->Write(stream, data, 0, &count);
    if (mode == STGM_READ)
        ok(ret == STG_E_ACCESSDENIED, "expected STG_E_ACCESSDENIED, got 0x%08x\n", ret);
    else
        ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    strcpy(data, "Hello");
    ret = stream->lpVtbl->Write(stream, data, 3, &count);
    if (mode == STGM_READ)
        ok(ret == STG_E_ACCESSDENIED, "expected STG_E_ACCESSDENIED, got 0x%08x\n", ret);
    else
        ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    /* IStream::Seek */

    ret = IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
    ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    ret = IStream_Seek(stream, zero, 20, NULL);
    ok(ret == E_INVALIDARG, "expected E_INVALIDARG, got 0x%08x\n", ret);

    /* IStream::CopyTo */

    ret = IStream_CopyTo(stream, NULL, uzero, &uret, &uret);
    ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    clone = NULL;
    ret = IStream_CopyTo(stream, clone, uzero, &uret, &uret);
    ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    ret = IStream_CopyTo(stream, stream, uzero, &uret, &uret);
    ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    ret = IStream_CopyTo(stream, stream, uzero, &uret, NULL);
    ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    ret = IStream_CopyTo(stream, stream, uzero, NULL, &uret);
    ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    /* IStream::Commit */

    ret = IStream_Commit(stream, STGC_DEFAULT);
    ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

    /* IStream::Revert */

    ret = IStream_Revert(stream);
    ok(ret == E_NOTIMPL, "expected E_NOTIMPL, got 0x%08x\n", ret);

    /* IStream::LockRegion */

    ret = IStream_LockRegion(stream, uzero, uzero, 0);
    ok(ret == E_NOTIMPL /* XP */ || ret == S_OK /* Vista */,
      "expected E_NOTIMPL or S_OK, got 0x%08x\n", ret);

    /* IStream::UnlockRegion */

    if (ret == E_NOTIMPL) /* XP */ {
        ret = IStream_UnlockRegion(stream, uzero, uzero, 0);
        ok(ret == E_NOTIMPL, "expected E_NOTIMPL, got 0x%08x\n", ret);
    } else /* Vista */ {
        ret = IStream_UnlockRegion(stream, uzero, uzero, 0);
        ok(ret == S_OK, "expected S_OK, got 0x%08x\n", ret);

        ret = IStream_UnlockRegion(stream, uzero, uzero, 0);
        ok(ret == STG_E_LOCKVIOLATION, "expected STG_E_LOCKVIOLATION, got 0x%08x\n", ret);
    }

    /* IStream::Stat */

    ret = IStream_Stat(stream, NULL, 0);
    ok(ret == STG_E_INVALIDPOINTER, "expected STG_E_INVALIDPOINTER, got 0x%08x\n", ret);

    /* IStream::Clone */

    ret = IStream_Clone(stream, NULL);
    ok(ret == E_NOTIMPL, "expected E_NOTIMPL, got 0x%08x\n", ret);

    clone = NULL;
    ret = IStream_Clone(stream, &clone);
    ok(ret == E_NOTIMPL, "expected E_NOTIMPL, got 0x%08x\n", ret);
    ok(clone == NULL, "expected a NULL IStream object, got %p\n", stream);

    if (clone) {
        refcount = IStream_Release(clone);
        ok(refcount == 0, "expected 0, got %d\n", refcount);
    }
}


static void test_SHCreateStreamOnFileA(DWORD mode)
{
    IStream * stream;
    HRESULT ret;
    ULONG refcount;
    static const char * test_file = "c:\\test.txt";

    printf("SHCreateStreamOnFileA: testing mode %d\n", mode);

    /* invalid arguments */

    stream = NULL;
    ret = (*pSHCreateStreamOnFileA)(NULL, mode, &stream);
    todo_wine
    ok(ret == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), "SHCreateStreamOnFileA: expected HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileA: expected a NULL IStream object, got %p\n", stream);

#if 0 /* This test crashes on WinXP SP2 */
    ret = (*pSHCreateStreamOnFileA)(test_file, mode, NULL);
    ok(ret == E_INVALIDARG, "SHCreateStreamOnFileA: expected E_INVALIDARG, got 0x%08x\n", ret);
#endif

    stream = NULL;
    ret = (*pSHCreateStreamOnFileA)(test_file, mode | STGM_CONVERT, &stream);
    ok(ret == E_INVALIDARG, "SHCreateStreamOnFileA: expected E_INVALIDARG, got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileA: expected a NULL IStream object, got %p\n", stream);

    stream = NULL;
    ret = (*pSHCreateStreamOnFileA)(test_file, mode | STGM_DELETEONRELEASE, &stream);
    ok(ret == E_INVALIDARG, "SHCreateStreamOnFileA: expected E_INVALIDARG, got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileA: expected a NULL IStream object, got %p\n", stream);

    stream = NULL;
    ret = (*pSHCreateStreamOnFileA)(test_file, mode | STGM_TRANSACTED, &stream);
    ok(ret == E_INVALIDARG, "SHCreateStreamOnFileA: expected E_INVALIDARG, got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileA: expected a NULL IStream object, got %p\n", stream);

    /* file does not exist */

    stream = NULL;
    ret = (*pSHCreateStreamOnFileA)(test_file, mode | STGM_FAILIFTHERE, &stream);
    todo_wine
    ok(ret == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "SHCreateStreamOnFileA: expected HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileA: expected a NULL IStream object, got %p\n", stream);

    stream = NULL;
    ret = (*pSHCreateStreamOnFileA)(test_file, mode | STGM_CREATE, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileA: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileA: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileA: expected 0, got %d\n", refcount);
    }

    /* NOTE: don't delete the file, as it will be used for the file exists tests. */

    /* file exists */

    stream = NULL;
    ret = (*pSHCreateStreamOnFileA)(test_file, mode | STGM_FAILIFTHERE, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileA: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileA: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileA: expected 0, got %d\n", refcount);
    }

    stream = NULL;
    ret = (*pSHCreateStreamOnFileA)(test_file, mode | STGM_CREATE, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileA: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileA: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileA: expected 0, got %d\n", refcount);

        ok(DeleteFileA(test_file), "SHCreateStreamOnFileA: could not delete file '%s', got error %d\n", test_file, GetLastError());
    }
}


static void test_SHCreateStreamOnFileW(DWORD mode)
{
    IStream * stream;
    HRESULT ret;
    ULONG refcount;
    static const WCHAR test_file[] = { 'c', ':', '\\', 't', 'e', 's', 't', '.', 't', 'x', 't' };

    printf("SHCreateStreamOnFileW: testing mode %d\n", mode);

    /* invalid arguments */

    stream = NULL;
    ret = (*pSHCreateStreamOnFileW)(NULL, mode, &stream);
    ok(ret == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) || /* XP */
       ret == E_INVALIDARG /* Vista */,
      "SHCreateStreamOnFileW: expected HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) or E_INVALIDARG, got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileW: expected a NULL IStream object, got %p\n", stream);

#if 0 /* This test crashes on WinXP SP2 */
    ret = (*pSHCreateStreamOnFileW)(test_file, mode, NULL);
    ok(ret == E_INVALIDARG, "SHCreateStreamOnFileW: expected E_INVALIDARG, got 0x%08x\n", ret);
#endif

    stream = NULL;
    ret = (*pSHCreateStreamOnFileW)(test_file, mode | STGM_CONVERT, &stream);
    ok(ret == E_INVALIDARG, "SHCreateStreamOnFileW: expected E_INVALIDARG, got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileW: expected a NULL IStream object, got %p\n", stream);

    stream = NULL;
    ret = (*pSHCreateStreamOnFileW)(test_file, mode | STGM_DELETEONRELEASE, &stream);
    ok(ret == E_INVALIDARG, "SHCreateStreamOnFileW: expected E_INVALIDARG, got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileW: expected a NULL IStream object, got %p\n", stream);

    stream = NULL;
    ret = (*pSHCreateStreamOnFileW)(test_file, mode | STGM_TRANSACTED, &stream);
    ok(ret == E_INVALIDARG, "SHCreateStreamOnFileW: expected E_INVALIDARG, got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileW: expected a NULL IStream object, got %p\n", stream);

    /* file does not exist */

    stream = NULL;
    ret = (*pSHCreateStreamOnFileW)(test_file, mode | STGM_FAILIFTHERE, &stream);
    todo_wine
    ok(ret == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "SHCreateStreamOnFileW: expected HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileW: expected a NULL IStream object, got %p\n", stream);

    stream = NULL;
    ret = (*pSHCreateStreamOnFileW)(test_file, mode | STGM_CREATE, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileW: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileW: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileW: expected 0, got %d\n", refcount);
    }

    /* NOTE: don't delete the file, as it will be used for the file exists tests. */

    /* file exists */

    stream = NULL;
    ret = (*pSHCreateStreamOnFileW)(test_file, mode | STGM_FAILIFTHERE, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileW: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileW: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileW: expected 0, got %d\n", refcount);
    }

    stream = NULL;
    ret = (*pSHCreateStreamOnFileW)(test_file, mode | STGM_CREATE, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileW: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileW: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileW: expected 0, got %d\n", refcount);

        ok(DeleteFileW(test_file), "SHCreateStreamOnFileW: could not delete the test file, got error %d\n", GetLastError());
    }
}


static void test_SHCreateStreamOnFileEx(DWORD mode, DWORD stgm)
{
    IStream * stream;
    IStream * template = NULL;
    HRESULT ret;
    ULONG refcount;
    static const WCHAR test_file[] = { 'c', ':', '\\', 't', 'e', 's', 't', '.', 't', 'x', 't' };

    printf("SHCreateStreamOnFileEx: testing mode %d, STGM flags %08x\n", mode, stgm);

    /* invalid arguments */

    stream = NULL;
    ret = (*pSHCreateStreamOnFileEx)(NULL, mode, 0, FALSE, NULL, &stream);
    ok(ret == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) || /* XP */
       ret == E_INVALIDARG /* Vista */,
      "SHCreateStreamOnFileEx: expected HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) or E_INVALIDARG, got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileEx: expected a NULL IStream object, got %p\n", stream);

    stream = NULL;
    ret = (*pSHCreateStreamOnFileEx)(test_file, mode, 0, FALSE, template, &stream);
    todo_wine
    ok(ret == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "SHCreateStreamOnFileEx: expected HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileEx: expected a NULL IStream object, got %p\n", stream);

#if 0 /* This test crashes on WinXP SP2 */
    ret = (*pSHCreateStreamOnFileEx)(test_file, mode, 0, FALSE, NULL, NULL);
    ok(ret == E_INVALIDARG, "SHCreateStreamOnFileEx: expected E_INVALIDARG, got 0x%08x\n", ret);
#endif

    /* file does not exist */

    stream = NULL;
    ret = (*pSHCreateStreamOnFileEx)(test_file, mode | STGM_FAILIFTHERE | stgm, 0, FALSE, NULL, &stream);
    if ((stgm & STGM_TRANSACTED) == STGM_TRANSACTED && mode == STGM_READ) {
        ok(ret == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) /* XP */ || ret == E_INVALIDARG /* Vista */,
          "SHCreateStreamOnFileEx: expected E_INVALIDARG or HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), got 0x%08x\n", ret);

        if (ret == E_INVALIDARG) {
            printf("SHCreateStreamOnFileEx: STGM_TRANSACTED not supported in this configuration... skipping.\n");
            return;
        }
    } else {
        todo_wine
        ok(ret == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "SHCreateStreamOnFileEx: expected HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), got 0x%08x\n", ret);
    }
    ok(stream == NULL, "SHCreateStreamOnFileEx: expected a NULL IStream object, got %p\n", stream);

    stream = NULL;
    ret = (*pSHCreateStreamOnFileEx)(test_file, mode | STGM_FAILIFTHERE | stgm, 0, TRUE, NULL, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileEx: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileEx: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileEx: expected 0, got %d\n", refcount);

        ok(DeleteFileW(test_file), "SHCreateStreamOnFileEx: could not delete the test file, got error %d\n", GetLastError());
    }

    stream = NULL;
    ret = (*pSHCreateStreamOnFileEx)(test_file, mode | STGM_CREATE | stgm, 0, FALSE, NULL, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileEx: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileEx: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileEx: expected 0, got %d\n", refcount);

        ok(DeleteFileW(test_file), "SHCreateStreamOnFileEx: could not delete the test file, got error %d\n", GetLastError());
    }

    stream = NULL;
    ret = (*pSHCreateStreamOnFileEx)(test_file, mode | STGM_CREATE | stgm, 0, TRUE, NULL, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileEx: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileEx: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileEx: expected 0, got %d\n", refcount);
    }

    /* NOTE: don't delete the file, as it will be used for the file exists tests. */

    /* file exists */

    stream = NULL;
    ret = (*pSHCreateStreamOnFileEx)(test_file, mode | STGM_FAILIFTHERE | stgm, 0, FALSE, NULL, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileEx: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileEx: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileEx: expected 0, got %d\n", refcount);
    }

    stream = NULL;
    ret = (*pSHCreateStreamOnFileEx)(test_file, mode | STGM_FAILIFTHERE | stgm, 0, TRUE, NULL, &stream);
    todo_wine
    ok(ret == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS), "SHCreateStreamOnFileEx: expected HRESULT_FROM_WIN32(ERROR_FILE_EXISTS), got 0x%08x\n", ret);
    ok(stream == NULL, "SHCreateStreamOnFileEx: expected a NULL IStream object, got %p\n", stream);

    stream = NULL;
    ret = (*pSHCreateStreamOnFileEx)(test_file, mode | STGM_CREATE | stgm, 0, FALSE, NULL, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileEx: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileEx: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileEx: expected 0, got %d\n", refcount);
    }

    stream = NULL;
    ret = (*pSHCreateStreamOnFileEx)(test_file, mode | STGM_CREATE | stgm, 0, TRUE, NULL, &stream);
    todo_wine
    ok(ret == S_OK, "SHCreateStreamOnFileEx: expected S_OK, got 0x%08x\n", ret);
    todo_wine
    ok(stream != NULL, "SHCreateStreamOnFileEx: expected a valid IStream object, got NULL\n");

    if (stream) {
        test_IStream_invalid_operations(stream, mode);

        refcount = IStream_Release(stream);
        ok(refcount == 0, "SHCreateStreamOnFileEx: expected 0, got %d\n", refcount);
    }

    todo_wine
    ok(DeleteFileW(test_file), "SHCreateStreamOnFileEx: could not delete the test file, got error %d\n", GetLastError());
}


START_TEST(istream)
{
    static const DWORD stgm_access[] = {
        STGM_READ,
        STGM_WRITE,
        STGM_READWRITE
    };

    static const DWORD stgm_flags[] = {
        0,
        STGM_CONVERT,
        STGM_DELETEONRELEASE,
        STGM_CONVERT | STGM_DELETEONRELEASE,
        STGM_TRANSACTED | STGM_CONVERT,
        STGM_TRANSACTED | STGM_DELETEONRELEASE,
        STGM_TRANSACTED | STGM_CONVERT | STGM_DELETEONRELEASE
    };

    int i, j;

    hShlwapi = GetModuleHandleA("shlwapi.dll");

    pSHCreateStreamOnFileA = (void*)GetProcAddress(hShlwapi, "SHCreateStreamOnFileA");
    pSHCreateStreamOnFileW = (void*)GetProcAddress(hShlwapi, "SHCreateStreamOnFileW");
    pSHCreateStreamOnFileEx = (void*)GetProcAddress(hShlwapi, "SHCreateStreamOnFileEx");

    if (!pSHCreateStreamOnFileA)
        printf("SHCreateStreamOnFileA not found... those tests will be skipped.\n");

    if (!pSHCreateStreamOnFileW)
        printf("SHCreateStreamOnFileW not found... those tests will be skipped.\n");

    if (!pSHCreateStreamOnFileEx)
        printf("SHCreateStreamOnFileEx not found... those tests will be skipped.\n");

    for (i = 0; i != sizeof(stgm_access)/sizeof(stgm_access[0]); i++) {
        if (pSHCreateStreamOnFileA)
            test_SHCreateStreamOnFileA(stgm_access[i]);

        if (pSHCreateStreamOnFileW)
            test_SHCreateStreamOnFileW(stgm_access[i]);

        if (pSHCreateStreamOnFileEx) {
            for (j = 0; j != sizeof(stgm_flags)/sizeof(stgm_flags[0]); j++)
                test_SHCreateStreamOnFileEx(stgm_access[i], stgm_flags[j]);
        }
    }
}
