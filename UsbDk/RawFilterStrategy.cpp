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

/* This filter strategy simply sets the RawDeviceOK flag. */
/* As a bug work-around, it also supplies a DeviceText if */
/* the device itself doesn't supply one, just like the Hide strategy. */ 
 
#include "stdafx.h"

#include "HiderStrategy.h"
#include "trace.h"
#include "RawFilterStrategy.tmh"
#include "FilterDevice.h"
#include "ControlDevice.h"
#include "UsbDkNames.h"

NTSTATUS CUsbDkRawFilterStrategy::Create(CUsbDkFilterDevice *Owner)
{
    auto status = CUsbDkNullFilterStrategy::Create(Owner);
    if (NT_SUCCESS(status))
    {
        m_ControlDevice->RegisterHiddenDevice(*Owner);
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HIDER, "%!FUNC! Serial number for this device is %lu", Owner->GetSerialNumber());

    }

    return status;
}

void CUsbDkRawFilterStrategy::Delete()
{
    if (m_ControlDevice != nullptr)
    {
        m_ControlDevice->UnregisterHiddenDevice(*m_Owner);
    }

    CUsbDkNullFilterStrategy::Delete();
}

NTSTATUS CUsbDkRawFilterStrategy::PatchDeviceText(PIRP Irp)
{
    static const WCHAR UsbDkDeviceText[] = USBDK_DRIVER_NAME L" device";

    const WCHAR *Buffer = nullptr;
    SIZE_T Size = 0;
#if 0
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HIDER, "%!FUNC! Entry");
#endif
    PIO_STACK_LOCATION  irpStack = IoGetCurrentIrpStackLocation(Irp);
    switch (irpStack->Parameters.QueryDeviceText.DeviceTextType)
    {
    case DeviceTextDescription:
        if (Irp->IoStatus.Information == 0) {   /* leave original name if it exists */
            Buffer = &UsbDkDeviceText[0];
            Size = sizeof(UsbDkDeviceText);
        }
        break;
    default:
        break;
    }

    if (Buffer != nullptr)
    {
        auto Result = DuplicateStaticBuffer(Buffer, Size);
        if (Result != nullptr)
        {
            if (Irp->IoStatus.Information != 0)
                ExFreePool(reinterpret_cast<PVOID>(Irp->IoStatus.Information));

            Irp->IoStatus.Information = reinterpret_cast<ULONG_PTR>(Result);
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
    }
    return Irp->IoStatus.Status;
}

NTSTATUS CUsbDkRawFilterStrategy::PNPPreProcess(PIRP Irp)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);

TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HIDER, "%!FUNC! Got Irp 0x%x",irpStack->MinorFunction);

    switch (irpStack->MinorFunction)
    {
    case IRP_MN_QUERY_CAPABILITIES:
        return PostProcessOnSuccess(Irp,
                           [](PIRP Irp) -> NTSTATUS
                           {
                               auto irpStack = IoGetCurrentIrpStackLocation(Irp);
                               irpStack->Parameters.DeviceCapabilities.Capabilities->RawDeviceOK = 1;
                               irpStack->Parameters.DeviceCapabilities.Capabilities->Removable = 0;
TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HIDER, "%!FUNC! Set RawDeviceOK");
                               return STATUS_SUCCESS;
                           });
    case IRP_MN_QUERY_DEVICE_TEXT:
        return PostProcess(Irp,
                           [this](PIRP Irp, NTSTATUS Status) -> NTSTATUS
                           {
                               UNREFERENCED_PARAMETER(Status);
                               return PatchDeviceText(Irp);
                           });
    default:
        return CUsbDkNullFilterStrategy::PNPPreProcess(Irp);
    }
}
