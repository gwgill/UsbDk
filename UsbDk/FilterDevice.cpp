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
#include "FilterDevice.h"
#include "trace.h"
#include "FilterDevice.tmh"
#include "DeviceAccess.h"
#include "Registry.h"
#include "ControlDevice.h"
#include "UsbDkNames.h"

void CUsbDkChildDevice::Dump()
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Child device 0x%p:", m_PDO);
    m_DeviceID->Dump();
    m_InstanceID->Dump();
}

class CUsbDkFilterDeviceInit : public CPreAllocatedDeviceInit
{
public:
    CUsbDkFilterDeviceInit(PWDFDEVICE_INIT DeviceInit)
    { Attach(DeviceInit); }

    NTSTATUS Configure(ULONG InstanceNumber);

    CUsbDkFilterDeviceInit(const CUsbDkFilterDeviceInit&) = delete;
    CUsbDkFilterDeviceInit& operator= (const CUsbDkFilterDeviceInit&) = delete;
private:
    static CUsbDkFilterDevice::CStrategist &Strategy(WDFDEVICE Device)
    { return UsbDkFilterGetContext(Device)->UsbDkFilter->m_Strategy; }
};

NTSTATUS CUsbDkFilterDeviceInit::Configure(ULONG InstanceNumber)
{
    PAGED_CODE();

    WDF_OBJECT_ATTRIBUTES requestAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestAttributes, WDF_REQUEST_CONTEXT);
    requestAttributes.ContextSizeOverride = CUsbDkFilterDevice::CStrategist::GetRequestContextSize();

    SetRequestAttributes(requestAttributes);
    SetFilter();

    CString DeviceName;
    auto status = DeviceName.Create(TEXT("\\Device\\UsbDkFilter"), InstanceNumber);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Failed to allocate filter device name (%!STATUS!)", status);
        return status;
    }

    status = SetName(*DeviceName);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! SetName failed %!STATUS!", status);
        return status;
    }

    SetPowerCallbacks([](_In_ WDFDEVICE Device)
                      { return Strategy(Device)->MakeAvailable(); });

    SetIoInCallerContextCallback([](_In_ WDFDEVICE Device, WDFREQUEST  Request)
                                 { return Strategy(Device)->IoInCallerContext(Device, Request); });

    SetFileEventCallbacks([](_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ WDFFILEOBJECT FileObject)
                          {
                                UNREFERENCED_PARAMETER(FileObject);
                                auto filter = UsbDkFilterGetContext(Device)->UsbDkFilter;
                                filter->m_open_count.AddRef();
                                filter->OnFileCreate(Request);
                          },
                          [](_In_ WDFFILEOBJECT FileObject)
                          {
                                WDFDEVICE Device = WdfFileObjectGetDevice(FileObject);
                                auto filter = UsbDkFilterGetContext(Device)->UsbDkFilter;
                                ULONG pid = 0; // zero means always match

                                // Check PID only if there are multiple open references to the file.
                                // If this was the last reference, always close the redirection.
                                //
                                // This callback function might run in a different process-context
                                // than the initiator process, therefore the 'current process ID'
                                // isn't always the ID of the 'owning' process.
                                //
                                // In the worst case, the USB redirection will be kept until the last
                                // open file handle to the device is closed.
                                //
                                // From KMDF 1.21, there's a new method that should give us the expected ID:
                                // WdfFileObjectGetInitiatorProcessId(FileObject)

                                if (filter->m_open_count.Release()) {
                                    pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
                                }
                                Strategy(Device)->OnClose(pid);
                          },
                          WDF_NO_EVENT_CALLBACK);

    status = SetPreprocessCallback([](_In_ WDFDEVICE Device, _Inout_  PIRP Irp)
                                        { return Strategy(Device)->PNPPreProcess(Irp); },
                                        IRP_MJ_PNP);

    return status;
}

NTSTATUS CUsbDkHubFilterStrategy::Create(CUsbDkFilterDevice *Owner)
{
    auto status = CUsbDkFilterStrategy::Create(Owner);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    m_ControlDevice->RegisterFilter(*Owner);
    return STATUS_SUCCESS;
}

void CUsbDkHubFilterStrategy::Delete()
{
    if (m_ControlDevice != nullptr)
    {
        m_ControlDevice->UnregisterFilter(*m_Owner);
    }

    CUsbDkFilterStrategy::Delete();
}

class CDeviceRelations
{
public:
    CDeviceRelations()
    {}

    virtual NTSTATUS Create(PDEVICE_RELATIONS Relations)
    {
        m_Relations = Relations;
        return STATUS_SUCCESS;
    }

    template <typename TPredicate, typename TFunctor>
    bool ForEachIf(TPredicate Predicate, TFunctor Functor) const
    {
        if (m_Relations != nullptr)
        {
            for (ULONG i = 0; i < m_Relations->Count; i++)
            {
                if (Predicate(m_Relations->Objects[i]) &&
                    !Functor(m_Relations->Objects[i]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    template <typename TPredicate>
    void WipeIf(TPredicate Predicate) const
    {
        if (m_Relations != nullptr)
        {
            ULONG WritePosition = 0;

            for (ULONG ReadPosition = 0; ReadPosition < m_Relations->Count; ReadPosition++)
            {
                if (!Predicate(m_Relations->Objects[ReadPosition]))
                {
                    m_Relations->Objects[WritePosition] = m_Relations->Objects[ReadPosition];
                    WritePosition++;
                }
            }

            m_Relations->Count = WritePosition;
        }
    }

    template <typename TFunctor>
    bool ForEach(TFunctor Functor) const
    { return ForEachIf(ConstTrue, Functor); }

    bool Contains(const CUsbDkChildDevice &Dev) const
    { return !ForEach([&Dev](PDEVICE_OBJECT Relation) { return !Dev.Match(Relation); }); }

    void Dump() const
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Array size: %lu (ptr: %p)",
                    (m_Relations != nullptr) ? m_Relations->Count : 0, m_Relations);

        ULONG Index = 0;
        ForEach([&Index](PDEVICE_OBJECT PDO) -> bool
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! #%lu: %p", Index++, PDO);
                    return true;
                });
    }
private:
    PDEVICE_RELATIONS m_Relations;

    CDeviceRelations(const CDeviceRelations&) = delete;
    CDeviceRelations& operator= (const CDeviceRelations&) = delete;
};

class CNonPagedDeviceRelations : public CDeviceRelations
{
public:
    virtual NTSTATUS Create(PDEVICE_RELATIONS Relations)
    {
        PDEVICE_RELATIONS NonPagedRelations = nullptr;

        if (Relations != nullptr)
        {
            m_PagedRelations = Relations;

            size_t RelationsSize = sizeof(DEVICE_RELATIONS) +
                                   sizeof(PDEVICE_OBJECT ) * Relations->Count -
                                   sizeof(PDEVICE_OBJECT);

            auto status = m_NonPagedCopy.Create(RelationsSize, USBDK_NON_PAGED_POOL);
            if (!NT_SUCCESS(status))
            {
                return status;
            }

            NonPagedRelations = static_cast<PDEVICE_RELATIONS>(m_NonPagedCopy.Ptr());
            RtlMoveMemory(NonPagedRelations, Relations, RelationsSize);
        }

        return CDeviceRelations::Create(NonPagedRelations);
    }

    ~CNonPagedDeviceRelations()
    {
        auto NonPagedRelations = static_cast<PDEVICE_RELATIONS>(m_NonPagedCopy.Ptr());

        if (NonPagedRelations != nullptr)
        {
            RtlMoveMemory(m_PagedRelations, NonPagedRelations, m_NonPagedCopy.Size());
        }
    }
private:
    CWdmMemoryBuffer m_NonPagedCopy;
    PDEVICE_RELATIONS m_PagedRelations = nullptr;
};

NTSTATUS CUsbDkHubFilterStrategy::PNPPreProcess(PIRP Irp)
{
    auto irpStack = IoGetCurrentIrpStackLocation(Irp);
    /*    GWG
      IRP_MN_START_DEVICE                   0x1
      IRP_MN_REMOVE_DEVICE                  0x2
      IRP_MN_QUERY_DEVICE_RELATIONS         0x7
      IRP_MN_QUERY_INTERFACE                0x8
      IRP_MN_QUERY_CAPABILITIES             0x9
      IRP_MN_QUERY_RESOURCES                0xa
      IRP_MN_QUERY_RESOURCE_REQUIREMENTS    0xb
      IRP_MN_QUERY_DEVICE_TEXT              0xc
      IRP_MN_QUERY_ID                       0x13
      IRP_MN_QUERY_PNP_DEVICE_STATE         0x14
      IRP_MN_QUERY_BUS_INFORMATION          0x15
      IRP_MN_SURPRISE_REMOVAL               0x17
      IRP_MN_DEVICE_ENUMERATED              0x19
     */
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HIDER, "%!FUNC! Irp 0x%x, PDO 0x%p",irpStack->MinorFunction, irpStack->DeviceObject);

    if ((irpStack->MinorFunction == IRP_MN_QUERY_DEVICE_RELATIONS) &&
        (BusRelations == irpStack->Parameters.QueryDeviceRelations.Type))
    {
        return PostProcessOnSuccess(Irp,
                                    [this](PIRP Irp) -> NTSTATUS
                                    {
                                        CNonPagedDeviceRelations Relations;
                                        auto status = Relations.Create((PDEVICE_RELATIONS)Irp->IoStatus.Information);

                                        if (!NT_SUCCESS(status))
                                        {
                                            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Failed to create device relations object: %!STATUS!", status);
                                            return STATUS_SUCCESS;
                                        }

                                        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Starting relations array processing:");
                                        Relations.Dump();

                                        DropRemovedDevices(Relations);
                                        AddNewDevices(Relations);
                                        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Finished relations array processing");
                                        return STATUS_SUCCESS;
                                    });
    }

    /* (From haidragon/hUsbDk) */
    /* Remove all the hubs children if it is unplugged. */
    if (irpStack->MinorFunction == IRP_MN_REMOVE_DEVICE)
    {
        DropAllDevices();
    }

    return CUsbDkFilterStrategy::PNPPreProcess(Irp);
}

void CUsbDkHubFilterStrategy::DropRemovedDevices(const CDeviceRelations &Relations)
{
    //Child device must be deleted on PASSIVE_LEVEL
    //So we put those to non-locked list and let its destructor do the job
    CWdmList<CUsbDkChildDevice, CRawAccess, CNonCountingObject> ToBeDeleted;
    Children().ForEachDetachedIf([&Relations](CUsbDkChildDevice *Child) { return !Relations.Contains(*Child); },
                                 [&ToBeDeleted](CUsbDkChildDevice *Child) -> bool
                                 {
                                     TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Deleting child object:");
                                     Child->Dump();
                                     ToBeDeleted.PushBack(Child);
                                     return true;
                                 });
    ToBeDeleted.ForEach([this](CUsbDkChildDevice *Device) -> bool
                        {
                            /* If the device is ReallyRaw, make it re-install on next plug */
                            if (Device->IfReallyRaw())
                                Device->MarkRawDeviceToReinstall();
                            m_ControlDevice->NotifyRedirectionRemoved(*Device);
                            return true;
                        });
}

void CUsbDkHubFilterStrategy::DropAllDevices()
{
    //Child device must be deleted on PASSIVE_LEVEL
    //So we put those to non-locked list and let its destructor do the job
    CWdmList<CUsbDkChildDevice, CRawAccess, CNonCountingObject> ToBeDeleted;
    Children().ForEachDetached([&ToBeDeleted](CUsbDkChildDevice *Child) -> bool
                               {
                                   TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Deleting child object:");
                                   Child->Dump();
                                   ToBeDeleted.PushBack(Child);
                                   return true;
                               });
    ToBeDeleted.ForEach([this](CUsbDkChildDevice *Device) -> bool
                        {
                            /* If the device is ReallyRaw, make it re-install on next plug */
                            if (Device->IfReallyRaw())
                                Device->MarkRawDeviceToReinstall();
                            m_ControlDevice->NotifyRedirectionRemoved(*Device);
                            return true;
                        });
}

bool CUsbDkHubFilterStrategy::IsChildRegistered(PDEVICE_OBJECT PDO)
{
    return !Children().ForEachIf([PDO](CUsbDkChildDevice *Child)
                                 {
                                     if (Child->Match(PDO))
                                     {
                                         TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! PDO %p already registered:", PDO);
                                         Child->Dump();
                                         return true;
                                     }
                                     return false;
                                 },
                                 ConstFalse);
}

void CUsbDkHubFilterStrategy::AddNewDevices(const CDeviceRelations &Relations)
{
    Relations.ForEachIf([this](PDEVICE_OBJECT PDO){ return !IsChildRegistered(PDO); },
                        [this](PDEVICE_OBJECT PDO){ RegisterNewChild(PDO); return true; });
}

void CUsbDkHubFilterStrategy::RegisterNewChild(PDEVICE_OBJECT PDO)
{
    CWdmUsbDeviceAccess pdoAccess(PDO);

    // in case when the device is composite and one of interfaces installed
    // under USB class and creates child device (example is composite USB with
    // more than one interface and one of them is CD) the PDO can be non-USB
    // device. Sending USB requests to it may be problematic as sending URB
    // is just internal device control with trivial IOCTL code.
    // Before trying to initialize it as USB device we check it is really one.
    CObjHolder<CRegText> DevID;
    CObjHolder<CRegText> InstanceID;
    CObjHolder<CRegText> LocationID;
    if (!UsbDkGetWdmDeviceIdentity(PDO, &DevID, &InstanceID, &LocationID))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Cannot query device identity");
        return;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Registering new child (PDO: %p):", PDO);
    DevID->Dump();
    InstanceID->Dump();
    LocationID->Dump();

    // Not a USB device -> do not register
    if (!DevID->MatchPrefix(L"USB\\"))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Not a usb device, skip child registration");
        return;
    }

    auto Port = pdoAccess.GetAddress();
    if (Port == CWdmDeviceAccess::NO_ADDRESS)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Cannot read device port number");
        return;
    }

    auto Speed = UsbDkWdmUsbDeviceGetSpeed(PDO, m_Owner->GetDriverObject());
    if (Speed == NoSpeed)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Cannot query device speed");
        return;
    }

    USB_DEVICE_DESCRIPTOR DevDescriptor;
    auto status = pdoAccess.GetDeviceDescriptor(DevDescriptor);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Cannot query device descriptor");
        return;
    }

#if (NTDDI_VERSION == NTDDI_WIN7)
    // recheck on Win7, superspeed indication as on Win8 might be not available
    if (Speed == HighSpeed && DevDescriptor.bcdUSB >= 0x300)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! superspeed assigned according to BCD field");
        Speed = SuperSpeed;
    }
#endif

    CUsbDkChildDevice::TDescriptorsCache CfgDescriptors(DevDescriptor.bNumConfigurations);

    if (!CfgDescriptors.Create())
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Cannot create descriptors cache");
        return;
    }

    if (!FetchConfigurationDescriptors(pdoAccess, CfgDescriptors))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Cannot fetch configuration descriptors");
        return;
    }

    CUsbDkChildDevice *Device = new CUsbDkChildDevice(DevID, InstanceID, LocationID, Port, Speed,
                                                    DevDescriptor, CfgDescriptors, *m_Owner, PDO);

    if (Device == nullptr)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Cannot allocate child device instance");
        return;
    }

    DevID.detach();
    InstanceID.detach();
    LocationID.detach();

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE,
        "%!FUNC! Adding child 0x%p PDO 0x%p", Device, PDO);
    Children().PushBack(Device);

    ApplyRedirectionPolicy(*Device);
}

bool CUsbDkHubFilterStrategy::FetchConfigurationDescriptors(CWdmUsbDeviceAccess &devAccess,
                                                            CUsbDkChildDevice::TDescriptorsCache &DescriptorsHolder)
{
    for (size_t i = 0; i < DescriptorsHolder.Size(); i++)
    {
        USB_CONFIGURATION_DESCRIPTOR Descriptor;
        auto status = devAccess.GetConfigurationDescriptor(static_cast<UCHAR>(i), Descriptor, sizeof(Descriptor));

        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Failed to read header for configuration descriptor %llu: %!STATUS!", i, status);
            return false;
        }

        if(!DescriptorsHolder.EmplaceEntry(i, Descriptor.wTotalLength,
                                           [&devAccess, &Descriptor, i](PUCHAR Buffer) -> bool
                                           {
                                                auto status = devAccess.GetConfigurationDescriptor(static_cast<UCHAR>(i),
                                                                                                   *reinterpret_cast<PUSB_CONFIGURATION_DESCRIPTOR>(Buffer),
                                                                                                   Descriptor.wTotalLength);
                                                if (!NT_SUCCESS(status))
                                                {
                                                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Failed to read configuration descriptor %llu: %!STATUS!", i, status);
                                                    return false;
                                                }
                                                return true;
                                           }))
        {
            return false;
        }
    }
    return true;
}

void CUsbDkHubFilterStrategy::ApplyRedirectionPolicy(CUsbDkChildDevice &Device)
{
    if (m_ControlDevice->ShouldRedirect(Device) ||
        m_ControlDevice->ShouldHideDevice(Device) ||
        m_ControlDevice->ShouldRawFiltDevice(Device, false))
    {
        if (Device.AttachToDeviceStack())
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Attached to device stack for 0x%p", Device.PDO());
        }
        else
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Failed to attach to device stack for 0x%p", Device.PDO());
        }
    }
    else
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Not attaching to device stack for 0x%p", Device.PDO());
    }
}

ULONG CUsbDkChildDevice::ParentID() const
{
    return m_ParentDevice.GetInstanceNumber();
}

bool CUsbDkChildDevice::AttachToDeviceStack()
{
    auto DriverObj = m_ParentDevice.GetDriverObject();

    auto Status = DriverObj->DriverExtension->AddDevice(DriverObj, m_PDO);
    if (!NT_SUCCESS(Status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Failed to attach to device stack");
        return false;
    }

    return true;
}

void CUsbDkFilterDevice::OnFileCreate(WDFREQUEST Request)
{
    CWdfRequest r(Request);
    WdfRequestFormatRequestUsingCurrentType(Request);
    // in the log we'll see which process created the file
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC!");
    r.SendWithCompletion(IOTarget(), [](WDFREQUEST Request, WDFIOTARGET Target, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
    {
        UNREFERENCED_PARAMETER(Target);
        CWdfRequest r(Request);
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%s: completed", (LPCSTR)Context);
        r.SetStatus(Params->IoStatus.Status);
        r.SetBytesWritten(Params->IoStatus.Information);
    },
    __FUNCTION__);
}

NTSTATUS CUsbDkFilterDevice::AttachToStack(WDFDRIVER Driver)
{
    PAGED_CODE();

    m_Driver = Driver;

    auto status = DefineStrategy();
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Attached, PDO 0x%p",WdmObject());
    return STATUS_SUCCESS;
}

NTSTATUS CUsbDkFilterDevice::DefineStrategy()
{
    m_Strategy.SetNullStrategy();
    auto status = m_Strategy->Create(this);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Failed to create null strategy");
        return status;
    }

    if (!m_Strategy.SelectStrategy(LowerDeviceObject()))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Not attached");
        return STATUS_NOT_SUPPORTED;
    }

    status = m_Strategy->Create(this);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Failed to create strategy");
        return status;
    }

    /* Tell the Hidden or Raw filter strategy what its child device PDO is */
    PDEVICE_OBJECT dpdo = IoGetLowerDeviceObject(WdmObject());
    while (dpdo != nullptr) {
        PDEVICE_OBJECT ndpdo = IoGetLowerDeviceObject(dpdo);
        if (ndpdo == nullptr)
        {
            m_Strategy->SetMyDevPDO(dpdo);    /* (Keeps reference until destructed) */
            break;
        }
        ObDereferenceObject(dpdo);
        dpdo = ndpdo;
    }

    return STATUS_SUCCESS;
}

NTSTATUS CUsbDkFilterDevice::Create(PWDFDEVICE_INIT DevInit)
{
    CUsbDkFilterDeviceInit DeviceInit(DevInit);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Entry");

    auto status = DeviceInit.Configure(GetInstanceNumber());
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Failed to create device init");
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, USBDK_FILTER_DEVICE_EXTENSION);
    attr.EvtCleanupCallback = CUsbDkFilterDevice::ContextCleanup;

    status = CWdfDevice::Create(DeviceInit, attr);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Failed to create device");
        return status;
    }
    if (!LowerDeviceObject())
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! No lower device, skip");
        return STATUS_INVALID_DEVICE_STATE;
    }

    auto deviceContext = UsbDkFilterGetContext(m_Device);
    deviceContext->UsbDkFilter = this;

    return STATUS_SUCCESS;
}

void CUsbDkFilterDevice::ContextCleanup(_In_ WDFOBJECT DeviceObject)
{
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Entry");

    auto deviceContext = UsbDkFilterGetContext(DeviceObject);
    deviceContext->UsbDkFilter->Release();
}

bool CUsbDkFilterDevice::CStrategist::SelectStrategy(PDEVICE_OBJECT DevObj)
{
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! for PDO 0x%p",DevObj);

    // Get device ID
    CObjHolder<CRegText> DevID;
    if (!UsbDkGetWdmDeviceIdentity(DevObj, &DevID, nullptr, nullptr))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Cannot query device ID");
        return false;
    }

    DevID->Dump();

    // Root hubs -> Hub strategy
    if ((DevID->Match(L"USB\\ROOT_HUB")         ||
         DevID->Match(L"USB\\ROOT_HUB20")       ||
         DevID->Match(L"USB\\ROOT_HUB30")       ||
         DevID->Match(L"USB\\USB20_HUB")        ||
         DevID->Match(L"USB\\USB30_HUB")        ||
         DevID->Match(L"USB\\ROOT_HUB30")       ||
         DevID->Match(L"NUSB3\\ROOT_HUB30")     ||
         DevID->Match(L"IUSB3\\ROOT_HUB30")     ||
         DevID->Match(L"IUSB3\\ROOT_HUB31")     ||
         DevID->Match(L"AMDUSB3\\ROOT_HUB3")    ||
         DevID->Match(L"AMDUSB3\\ROOT_HUB31")))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Assigning HUB strategy");
        m_Strategy->Delete();
        m_Strategy = &m_HubStrategy;
        return true;
    }

    // Not a USB device -> do not filter
    if (!DevID->MatchPrefix(L"USB\\"))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Not a usb device, no strategy assigned");
        return false;
    }

    // Get instance ID
    CObjHolder<CRegText> InstanceID;
    if (!UsbDkGetWdmDeviceIdentity(DevObj, nullptr, &InstanceID, nullptr))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! Cannot query instance ID");
        return false;
    }

    USB_DK_DEVICE_ID ID;
    UsbDkFillIDStruct(&ID, *DevID->begin(), *InstanceID->begin());

    // Get device descriptor
    USB_DEVICE_DESCRIPTOR DevDescr;
    if (!m_Strategy->GetControlDevice()->GetDeviceDescriptor(ID, DevDescr))
    {
        // If there is no cached device descriptor then we did not
        // attach to this device stack during parent bus enumeration
        // (see CUsbDkHubFilterStrategy::RegisterNewChild).
        // In this case, the fact that WDF called our
        // EVT_WDF_DRIVER_DEVICE_ADD callback for this device means that
        // we are dealing with USB hub and WDF attached us to its stack
        // automatically because UsbDk is registered in PNP manager as
        // USB hubs filter

        /* OR we failed to query the device descriptor, and really shouldn't treat it as a HUB! */
        /* (Failure to query the device descriptor appears to return VID_0000&PID_0002) */
        if (DevID->Match(L"USB\\VID_0000&PID_0002"))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! No cached device descriptor, and bad VID/PI, No strategy assigned");
            return false;
        }

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! No cached device descriptor, assigning hub strategy");
        m_Strategy->Delete();
        m_Strategy = &m_HubStrategy;
        return true;
    }

    // Device class is HUB -> Hub strategy
    if (DevDescr.bDeviceClass == USB_DEVICE_CLASS_HUB)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Device class is HUB, assigning hub strategy");
        m_Strategy->Delete();
        m_Strategy = &m_HubStrategy;
        return true;
    }

    // Configuration tells to redirect -> redirector strategy
    if (m_Strategy->GetControlDevice()->ShouldRedirect(ID))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Assigning redirected USB device strategy");
        m_DevStrategy.SetDeviceID(DevID.detach());
        m_DevStrategy.SetInstanceID(InstanceID.detach());
        m_Strategy->Delete();
        m_Strategy = &m_DevStrategy;
        return true;
    }

    // Should be hidden -> hider strategy
    if (m_Strategy->GetControlDevice()->ShouldHide(ID))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Assigning hidden USB device strategy");
        m_Strategy->Delete();
        m_Strategy = &m_HiderStrategy;
        return true;
    }

    // Mark as Raw strategy to allow Redirect CYCLE_PORT to work.
    if (m_Strategy->GetControlDevice()->ShouldRawFilt(ID))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Assigning Raw filer device strategy");
        m_Strategy->Delete();
        m_Strategy = &m_RawFilterStrategy;
        return true;
    }

    // No strategy
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "%!FUNC! Do not redirect or already redirected device, no strategy assigned");

    return false;
}

size_t CUsbDkFilterDevice::CStrategist::GetRequestContextSize()
{
    return max(CUsbDkNullFilterStrategy::GetRequestContextSize(),
               max(CUsbDkHubFilterStrategy::GetRequestContextSize(),
                   CUsbDkRedirectorStrategy::GetRequestContextSize()));
}

static ULONG InterfaceTypeMask(UCHAR bClass) {
    switch (bClass) {
    case USB_DEVICE_CLASS_AUDIO:
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "Class 0x%X -> audio", bClass);
        return 1 << USB_DEVICE_CLASS_AUDIO;
    case USB_DEVICE_CLASS_COMMUNICATIONS:
    case USB_DEVICE_CLASS_CDC_DATA:
    case USB_DEVICE_CLASS_WIRELESS_CONTROLLER:
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "Class 0x%X -> network", bClass);
        return 1 << USB_DEVICE_CLASS_COMMUNICATIONS;
    case USB_DEVICE_CLASS_PRINTER:
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "Class 0x%X -> printer", bClass);
        return 1 << USB_DEVICE_CLASS_PRINTER;
    case USB_DEVICE_CLASS_STORAGE:
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "Class 0x%X -> storage", bClass);
        return 1 << USB_DEVICE_CLASS_STORAGE;
    case USB_DEVICE_CLASS_VIDEO:
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "Class 0x%X -> video", bClass);
        return 1 << USB_DEVICE_CLASS_VIDEO;
    case USB_DEVICE_CLASS_AUDIO_VIDEO:
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "Class 0x%X -> audio/video", bClass);
        return (1 << USB_DEVICE_CLASS_VIDEO) |
               (1 << USB_DEVICE_CLASS_AUDIO);
    case USB_DEVICE_CLASS_HUB:
        return (1 << USB_DEVICE_CLASS_HUB);
    case USB_DEVICE_CLASS_HUMAN_INTERFACE:
        return (1 << USB_DEVICE_CLASS_HUMAN_INTERFACE);
    default:
        return 1U << 31;
    }
}

static PUSB_INTERFACE_DESCRIPTOR FindNextInterface(PUSB_CONFIGURATION_DESCRIPTOR cfg, ULONG& offset)
{
    PUSB_COMMON_DESCRIPTOR desc;
    if (offset >= cfg->wTotalLength)
        return NULL;
    do {
        if (offset + sizeof(*desc) > cfg->wTotalLength)
            return NULL;
        desc = (PUSB_COMMON_DESCRIPTOR)((PUCHAR)cfg + offset);
        if (desc->bLength + offset > cfg->wTotalLength)
            return NULL;
        offset += desc->bLength;
        if (desc->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE) {
            return (PUSB_INTERFACE_DESCRIPTOR)desc;
        }
    } while (1);
}

void CUsbDkChildDevice::DetermineDeviceClasses()
{
    if (m_DevDescriptor.bDeviceClass)
    {
        m_ClassMaskForExtHider |= InterfaceTypeMask(m_DevDescriptor.bDeviceClass);
    }

    USB_CONFIGURATION_DESCRIPTOR cfg;
    for (UCHAR index = 0; index < m_CfgDescriptors.Size(); ++index)
    {
        if (ConfigurationDescriptor(index, cfg, sizeof(cfg)))
        {
            PVOID buffer = ExAllocatePoolWithTag(USBDK_NON_PAGED_POOL, cfg.wTotalLength, 'CFGD');
            if (buffer)
            {
                USB_CONFIGURATION_DESCRIPTOR *p = (USB_CONFIGURATION_DESCRIPTOR *)buffer;
                if (ConfigurationDescriptor(index, *p, cfg.wTotalLength))
                {
                    ULONG offset = 0;
                    PUSB_INTERFACE_DESCRIPTOR intf_desc;
                    while (true)
                    {
                        intf_desc = FindNextInterface(p, offset);
                        if (!intf_desc)
                            break;
                        m_ClassMaskForExtHider |= InterfaceTypeMask(intf_desc->bInterfaceClass);
                    }
                }
                ExFreePool(buffer);
            }
        }
    }
#define SINGLE_DETERMINATIVE_CLASS
#if defined(SINGLE_DETERMINATIVE_CLASS)
    // only one determinative type present in final mask
    if (m_ClassMaskForExtHider & (1 << USB_DEVICE_CLASS_PRINTER))
    {
        m_ClassMaskForExtHider = 1 << USB_DEVICE_CLASS_PRINTER;
    }
    else if (m_ClassMaskForExtHider & (1 << USB_DEVICE_CLASS_COMMUNICATIONS))
    {
        m_ClassMaskForExtHider = 1 << USB_DEVICE_CLASS_COMMUNICATIONS;
    }
    else if (m_ClassMaskForExtHider & ((1 << USB_DEVICE_CLASS_AUDIO) | (1 << USB_DEVICE_CLASS_VIDEO)))
    {
        m_ClassMaskForExtHider &= (1 << USB_DEVICE_CLASS_AUDIO) | (1 << USB_DEVICE_CLASS_VIDEO);
    }
#else
    // all the determinative types present in final mask
    ULONG determinativeMask =
        (1 << USB_DEVICE_CLASS_PRINTER) |
        (1 << USB_DEVICE_CLASS_COMMUNICATIONS) |
        (1 << USB_DEVICE_CLASS_AUDIO) |
        (1 << USB_DEVICE_CLASS_VIDEO);
    if (m_ClassMaskForExtHider & determinativeMask)
    {
        m_ClassMaskForExtHider &= determinativeMask;
    }
#endif
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FILTERDEVICE, "Class mask %08X", m_ClassMaskForExtHider);
}

/* We can call this once the Raw Filtered device is real to see if our */
/* guess as to whether the device has a function driver is corrrect. */
bool CUsbDkChildDevice::IfReallyRaw() {
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! m_IsProbablyRaw %d, m_HaveCheckedIfReallyRaw %d",m_IsProbablyRaw,m_HaveCheckedIfReallyRaw);

    /* We only have to make this check once */
    if (!m_IsProbablyRaw || m_HaveCheckedIfReallyRaw)
    {
        m_HaveCheckedIfReallyRaw = true;

        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! returning %d",m_IsReallyRaw);
        return m_IsReallyRaw;
    }

    m_HaveCheckedIfReallyRaw = true;

    CRegKey regkey;
       
    auto status = regkey.Open(*m_HwKeyPath);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
            "%!FUNC! Failed to open Key '%wZ' registry key",m_HwKeyPath);
        return m_IsReallyRaw;        /* Hmm. */
    }
    CStringHolder DriverNameHolder;
    status = DriverNameHolder.Attach(TEXT("Driver"));
    ASSERT(NT_SUCCESS(status));
    
    CWdmMemoryBuffer Buffer;
    status = regkey.QueryValueInfo(*DriverNameHolder, KeyValuePartialInformation, Buffer);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
            "%!FUNC! Failed to read value '%wZ\\Driver' (status %!STATUS!)",
                                                   m_HwKeyPath, status);
        m_IsReallyRaw = true;
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
            "%!FUNC! As able to read value '%wZ\\Driver'", m_HwKeyPath);
        m_IsReallyRaw = false;
    }

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! returning %d",m_IsReallyRaw);
    return m_IsReallyRaw;
}

#ifndef CONFIGFLAG_REINSTALL        /* This is in um/RegStr.h */
# define CONFIGFLAG_REINSTALL            0x00000020      // Redo install
#endif

void CUsbDkChildDevice::MarkRawDeviceToReinstall()
{
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FILTERDEVICE, "%!FUNC! setting reinstall flag "
                                                         "on Child PDO 0x%p",PDO());
    CRegKey regkey;
       
    auto status = regkey.Open(*m_HwKeyPath);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
            "%!FUNC! Failed to open Key '%wZ' registry key",m_HwKeyPath);
        return;
    }
    CStringHolder ConfigFlagsNameHolder;
    status = ConfigFlagsNameHolder.Attach(TEXT("ConfigFlags"));
    ASSERT(NT_SUCCESS(status));
    
    CWdmMemoryBuffer Buffer;
    status = regkey.QueryValueInfo(*ConfigFlagsNameHolder, KeyValuePartialInformation, Buffer);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
            "%!FUNC! Failed to read value '%wZ\\ConfigFlags' (status %!STATUS!)",
                                                   m_HwKeyPath, status);
        return;
    }
       auto Info = reinterpret_cast<PKEY_VALUE_PARTIAL_INFORMATION>(Buffer.Ptr());

    if (Info->Type != REG_DWORD
     || Info->DataLength != sizeof(DWORD32))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE,
            "%!FUNC! Wrong data type/length for value %wZ\\ConfigFlags", m_HwKeyPath);
        return;
    }
    DWORD32 *flags = reinterpret_cast<DWORD32 *>(Info->Data);
    *flags |= CONFIGFLAG_REINSTALL;

    status = regkey.SetValueInfo(*ConfigFlagsNameHolder, Info);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE,
               "%!FUNC! Failed to update %wZ\\ConfigFlags (status %!STATUS!)",
                                                  m_HwKeyPath, status);
    }
}

