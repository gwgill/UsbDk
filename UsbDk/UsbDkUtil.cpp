/**********************************************************************
* Copyright (c) 2013-2014  Red Hat, Inc.
*
* Developed by Daynix Computing LTD.
*
* Authors:
*     Dmitry Fleytman <dmitry@daynix.com>
*     Pavel Gurvich <pavel@daynix.com>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
**********************************************************************/

#include "stdafx.h"
#include "Alloc.h"
#include "UsbDkUtil.h"
#include "Trace.h"
#include "UsbDkUtil.tmh"

NTSTATUS CWdmEvent::Wait(bool WithTimeout, LONGLONG Timeout, bool Alertable)
{
    LARGE_INTEGER PackedTimeout;
    PackedTimeout.QuadPart = Timeout;

    return KeWaitForSingleObject(&m_Event,
                                 Executive,
                                 KernelMode,
                                 Alertable ? TRUE : FALSE,
                                 WithTimeout ? &PackedTimeout : nullptr);
}

NTSTATUS CString::Resize(USHORT NewLenBytes)
{
    auto NewBuff = static_cast<PWCH>(ExAllocatePoolWithTag(USBDK_NON_PAGED_POOL,
                                                           NewLenBytes,
                                                           'SUHR'));
    if (NewBuff == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_String.Length = min(m_String.Length, NewLenBytes);
    m_String.MaximumLength = NewLenBytes;

    if (m_String.Buffer != nullptr)
    {
        RtlCopyMemory(NewBuff, m_String.Buffer, m_String.Length);
        ExFreePoolWithTag(m_String.Buffer, 'SUHR');
    }

    m_String.Buffer = NewBuff;

    return STATUS_SUCCESS;
}

NTSTATUS CString::Create(NTSTRSAFE_PCWSTR String)
{
    UNICODE_STRING StringHolder;
    auto status = RtlUnicodeStringInit(&StringHolder, String);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    return Create(&StringHolder);
}

NTSTATUS CString::Create(PCUNICODE_STRING String)
{
    m_String.MaximumLength = String->MaximumLength;
    m_String.Buffer = static_cast<PWCH>(ExAllocatePoolWithTag(USBDK_NON_PAGED_POOL,
        String->MaximumLength,
        'SUHR'));

    if (m_String.Buffer == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyUnicodeString(&m_String, String);
    return STATUS_SUCCESS;
}

NTSTATUS CString::Append(PCUNICODE_STRING String)
{
    auto status = Resize(m_String.Length + String->Length);
    if (NT_SUCCESS(status))
    {
        status = RtlAppendUnicodeStringToString(&m_String, String);
    }

    return status;
}

NTSTATUS CString::Append(ULONG Num, ULONG Base)
{
    //Allocating buffer big enough to hold
    //binary representation of ULONG
    static const int BitsInByte = 8;
    WCHAR SzNumber[sizeof(ULONG)* BitsInByte];

    CStringHolder StrNumber;
    StrNumber.Attach(SzNumber, sizeof(SzNumber));
    StrNumber.ToString(Num, Base);

    return Append(StrNumber);
}

void CString::Destroy()
{
    if (m_String.Buffer != nullptr)
    {
        m_String.Length = 0;
        m_String.MaximumLength = 0;
        ExFreePoolWithTag(m_String.Buffer, 'SUHR');
        m_String.Buffer = nullptr;
    }
}

bool CStringBase::operator== (const UNICODE_STRING& Str) const
{
    if (m_String.Length != Str.Length)
    {
        return false;
    }

    return RtlEqualMemory(m_String.Buffer, Str.Buffer, Str.Length);
}

size_t CStringBase::ToWSTR(PWCHAR Buffer, size_t SizeBytes) const
{
    size_t BytesToCopy = min(SizeBytes - sizeof(Buffer[0]), m_String.Length);
    RtlCopyMemory(Buffer, m_String.Buffer, BytesToCopy);
    Buffer[BytesToCopy / sizeof(Buffer[0])] = L'\0';
    return BytesToCopy + sizeof(Buffer[0]);
}

/* Truncate at the end of the pattern string. */
/* Return true on success */
bool CStringBase::TruncateAfter(PCWSTR patn)
{
    size_t off, len, plen;

    const int bpwc = sizeof(WCHAR);
    len = m_String.Length/bpwc;
    plen = wcslen(patn);

    if (plen == 0 || len < plen)
        return false;

    for (off = 0; len >= plen; off++, len--) {

        /* Look for first char match */
        if (patn[0] != m_String.Buffer[off])
            continue;

        /* See if the strings match */
        if (memcmp(&m_String.Buffer[off], patn, bpwc * plen) == 0) {
            m_String.Length = static_cast<USHORT>(bpwc * (off + plen));    /* Truncate */
            return true;
        }
    }
    return false;
}

/* Do a string match with possible character '?' wilcards in pattern */
/* Return true on match */
bool CStringBase::WCMatch(PCWSTR patn)
{
    size_t off, len, plen;

    len = m_String.Length/sizeof(WCHAR);
    plen = wcslen(patn);

    if (len > 0 && m_String.Buffer[len-1] == L'\0')
        len--;

    if (len != plen)
        return false;

    for (off = 0; off < len; off++) {
        if (patn[off] != m_String.Buffer[off]
         && patn[off] != L'?')
            return false;
    }
    return true;
}

PVOID DuplicateStaticBuffer(const void *Buffer, SIZE_T Length, POOL_TYPE PoolType)
{
    ASSERT(Buffer != nullptr);
    ASSERT(Length > 0);

    auto Duplicate = ExAllocatePoolWithTag(PoolType, Length, 'BDHR');
    if (Duplicate != nullptr)
    {
        RtlCopyMemory(Duplicate, Buffer, Length);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_UTILS, "%!FUNC! Failed to allocate buffer");
    }
    return Duplicate;
}

NTSTATUS
UsbDkCreateCurrentProcessHandle(HANDLE &Handle)
{
    PAGED_CODE();

    auto status = ZwDuplicateObject(ZwCurrentProcess(),
                                    ZwCurrentProcess(),
                                    ZwCurrentProcess(),
                                    &Handle,
                                    PROCESS_DUP_HANDLE,
                                    OBJ_KERNEL_HANDLE,
                                    0);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_UTILS, "%!FUNC! failed, error: %!STATUS!", status);
    }

    return status;
}

CStopWatch& CStopWatch::operator= (const CStopWatch& Other)
{
    m_StartTime = Other.m_StartTime;
    return *this;
}

LONGLONG CStopWatch::Time100Ns() const
{
    LARGE_INTEGER Now;
    KeQueryTickCount(&Now);
    return (Now.QuadPart - m_StartTime.QuadPart) * m_TimeIncrement;
}

/* Convert four Unicode Hex characters at an offset in a string, into an 16 bit value */
static NTSTATUS FourHexToInteger(
    PCUNICODE_STRING String,    /* String to read from */
    USHORT             Off,        /* Character offset into string of 4 characters */
    PULONG           Value        /* Output */
) {
    const int bpwc = sizeof(WCHAR);
    UNICODE_STRING SubString;

    SubString.Length = bpwc * 4;
    SubString.MaximumLength = String->Length - bpwc * Off;
    SubString.Buffer = String->Buffer + Off;

    if (SubString.MaximumLength < 4)
        return STATUS_INVALID_PARAMETER;

    return RtlUnicodeStringToInteger(&SubString, 16, Value);
}

/* Convert 2x4 digit hex unicode characters into a 32 bit value */
NTSTATUS EightHexToInteger(
    PCUNICODE_STRING String,    /* String to read from */
    USHORT             MsOff,        /* Char offset into string of MS characters */
    USHORT             LsOff,        /* Char offset into string of LS characters */
    PULONG           Value        /* Output */
) {
    NTSTATUS status;
    ULONG tval1, tval2;

    if ((status = FourHexToInteger(String, MsOff, &tval1)) != STATUS_SUCCESS)
        return status;
    if ((status = FourHexToInteger(String, LsOff, &tval2)) != STATUS_SUCCESS)
        return status;

    *Value = (tval1 << 16) + tval2;

    return status;
}

/* Convert 2x4 digit hex unicode characters into a 32 bit value, Sz version. */
NTSTATUS EightHexToInteger(
    PCWCHAR          String,    /* String to read from */
    USHORT             MsOff,        /* Char offset into string of MS characters */
    USHORT             LsOff,        /* Char offset into string of LS characters */
    PULONG           Value        /* Output */
) {
    const int bpwc = sizeof(WCHAR);
    UNICODE_STRING UCString;
    size_t slen = wcslen(String);

    if (slen > (bpwc * ((1 << (sizeof(USHORT) * 8))-1)))
        return STATUS_INVALID_PARAMETER;

    UCString.Length = static_cast<USHORT>(bpwc * slen);
    UCString.MaximumLength = UCString.Length;
    UCString.Buffer = const_cast<PWCH>(String);        /* UCString becomes const, so safe. */

    return EightHexToInteger(&UCString, MsOff, LsOff, Value);
}

