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
#include "ControlDevice.h"
#include "trace.h"
#include "DeviceAccess.h"
#include "Registry.h"
#include "ControlDevice.tmh"
#include "Public.h"

class CUsbDkControlDeviceInit : public CDeviceInit
{
public:
    CUsbDkControlDeviceInit()
    {}

    NTSTATUS Create(WDFDRIVER Driver);

    CUsbDkControlDeviceInit(const CUsbDkControlDeviceInit&) = delete;
    CUsbDkControlDeviceInit& operator= (const CUsbDkControlDeviceInit&) = delete;
};

NTSTATUS CUsbDkControlDeviceInit::Create(WDFDRIVER Driver)
{
    auto DeviceInit = WdfControlDeviceInitAllocate(Driver, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RWX_RES_RWX);
    if (DeviceInit == nullptr)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! Cannot allocate DeviceInit");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Attach(DeviceInit);

    WDF_OBJECT_ATTRIBUTES requestAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestAttributes, USBDK_CONTROL_REQUEST_CONTEXT);
    requestAttributes.ContextSizeOverride = sizeof(USBDK_CONTROL_REQUEST_CONTEXT);

    SetRequestAttributes(requestAttributes);

    SetExclusive();
    SetIoBuffered();

    SetIoInCallerContextCallback(CUsbDkControlDevice::IoInCallerContext);

    DECLARE_CONST_UNICODE_STRING(ntDeviceName, USBDK_DEVICE_NAME);
    return SetName(ntDeviceName);
}

void CUsbDkControlDeviceQueue::SetCallbacks(WDF_IO_QUEUE_CONFIG &QueueConfig)
{
    QueueConfig.EvtIoDeviceControl = CUsbDkControlDeviceQueue::DeviceControl;
}

void CUsbDkControlDeviceQueue::DeviceControl(WDFQUEUE Queue,
                                             WDFREQUEST Request,
                                             size_t OutputBufferLength,
                                             size_t InputBufferLength,
                                             ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    CControlRequest WdfRequest(Request);

    switch (IoControlCode)
    {
        case IOCTL_USBDK_COUNT_DEVICES:
        {
            CountDevices(WdfRequest, Queue);
            break;
        }
        case IOCTL_USBDK_ENUM_DEVICES:
        {
            EnumerateDevices(WdfRequest, Queue);
            break;
        }
        case IOCTL_USBDK_GET_CONFIG_DESCRIPTOR:
        {
            GetConfigurationDescriptor(WdfRequest, Queue);
            break;
        }
        case IOCTL_USBDK_UPDATE_REG_PARAMETERS:
        {
            UpdateRegistryParameters(WdfRequest, Queue);
            break;
        }
        case IOCTL_USBDK_ADD_REDIRECT:
        {
            AddRedirect(WdfRequest, Queue);
            break;
        }
        default:
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "Wrong IoControlCode 0x%X\n", IoControlCode);
            WdfRequest.SetStatus(STATUS_INVALID_DEVICE_REQUEST);
            break;
        }
    }
}

void CUsbDkControlDeviceQueue::CountDevices(CControlRequest &Request, WDFQUEUE Queue)
{
    ULONG *numberDevices;
    auto status = Request.FetchOutputObject(numberDevices);
    if (NT_SUCCESS(status))
    {
        auto devExt = UsbDkControlGetContext(WdfIoQueueGetDevice(Queue));
        *numberDevices = devExt->UsbDkControl->CountDevices();

        Request.SetOutputDataLen(sizeof(*numberDevices));
    }

    Request.SetStatus(status);
}

void CUsbDkControlDeviceQueue::UpdateRegistryParameters(CControlRequest &Request, WDFQUEUE Queue)
{
    auto devExt = UsbDkControlGetContext(WdfIoQueueGetDevice(Queue));
    auto status = devExt->UsbDkControl->RescanRegistry();
    Request.SetStatus(status);
}

void CUsbDkControlDeviceQueue::EnumerateDevices(CControlRequest &Request, WDFQUEUE Queue)
{
    USB_DK_DEVICE_INFO *existingDevices;
    size_t  numberExistingDevices = 0;
    size_t numberAllocatedDevices;

    auto status = Request.FetchOutputArray(existingDevices, numberAllocatedDevices);
    if (!NT_SUCCESS(status))
    {
        Request.SetStatus(status);
        return;
    }

    auto devExt = UsbDkControlGetContext(WdfIoQueueGetDevice(Queue));
    auto res = devExt->UsbDkControl->EnumerateDevices(existingDevices, numberAllocatedDevices, numberExistingDevices);

    if (res)
    {
        Request.SetOutputDataLen(numberExistingDevices * sizeof(USB_DK_DEVICE_INFO));
        Request.SetStatus(STATUS_SUCCESS);
    }
    else
    {
        Request.SetStatus(STATUS_BUFFER_TOO_SMALL);
    }
}

void CUsbDkControlDeviceQueue::AddRedirect(CControlRequest &Request, WDFQUEUE Queue)
{
    NTSTATUS status;

    PUSB_DK_DEVICE_ID DeviceId;
    PULONG64 RedirectorDevice;

    auto TargetProcessHandle = Request.Context()->CallerProcessHandle;
    if (TargetProcessHandle != WDF_NO_HANDLE)
    {
        if (FetchBuffersForAddRedirectRequest(Request, DeviceId, RedirectorDevice))
        {
            auto devExt = UsbDkControlGetContext(WdfIoQueueGetDevice(Queue));
            status = devExt->UsbDkControl->AddRedirect(*DeviceId, TargetProcessHandle, reinterpret_cast<PHANDLE>(RedirectorDevice));
        }
        else
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }

        ZwClose(TargetProcessHandle);
    }
    else
    {
        //May happen if IoInCallerContext callback
        //wasn't called due to low memory conditions
        status = STATUS_INSUFFICIENT_RESOURCES;
    }

    Request.SetOutputDataLen(NT_SUCCESS(status) ? sizeof(*RedirectorDevice) : 0);
    Request.SetStatus(status);
}

template <typename TInputObj, typename TOutputObj>
static void CUsbDkControlDeviceQueue::DoUSBDeviceOp(CControlRequest &Request,
                                                    WDFQUEUE Queue,
                                                    USBDevControlMethodWithOutput<TInputObj, TOutputObj> Method)
{
    TOutputObj *output;
    size_t outputLength;

    auto status = Request.FetchOutputObject(output, &outputLength);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Failed to fetch output buffer. %!STATUS!", status);
        Request.SetOutputDataLen(0);
        Request.SetStatus(status);
        return;
    }

    TInputObj *input;
    status = Request.FetchInputObject(input);
    if (NT_SUCCESS(status))
    {
        auto devExt = UsbDkControlGetContext(WdfIoQueueGetDevice(Queue));
        status = (devExt->UsbDkControl->*Method)(*input, output, &outputLength);
    }

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Fail with code: %!STATUS!", status);
    }

    Request.SetOutputDataLen(outputLength);
    Request.SetStatus(status);
}

void CUsbDkControlDeviceQueue::DoUSBDeviceOp(CControlRequest &Request, WDFQUEUE Queue, USBDevControlMethod Method)
{
    USB_DK_DEVICE_ID *deviceId;
    auto status = Request.FetchInputObject(deviceId);
    if (NT_SUCCESS(status))
    {
        auto devExt = UsbDkControlGetContext(WdfIoQueueGetDevice(Queue));
        status = (devExt->UsbDkControl->*Method)(*deviceId);
    }

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Fail with code: %!STATUS!", status);
    }

    Request.SetStatus(status);
}

void CUsbDkControlDeviceQueue::GetConfigurationDescriptor(CControlRequest &Request, WDFQUEUE Queue)
{
    DoUSBDeviceOp<USB_DK_CONFIG_DESCRIPTOR_REQUEST, USB_CONFIGURATION_DESCRIPTOR>(Request, Queue, &CUsbDkControlDevice::GetConfigurationDescriptor);
}

ULONG CUsbDkControlDevice::CountDevices()
{
    ULONG numberDevices = 0;

    m_FilterDevices.ForEach([&numberDevices](CUsbDkFilterDevice *Filter)
    {
        numberDevices += Filter->GetChildrenCount();
        return true;
    });

    return numberDevices;
}

void CUsbDkControlDevice::RegisterHiddenDevice(CUsbDkFilterDevice &FilterDevice)
{
    // Windows recognizes USB devices by PID/VID/SN combination,
    // for each new USB device plugged in it generates a new
    // cached driver information entry in the registry.
    //
    // When UsbDk hides or redirects a device it creates a virtual
    // device with UsbDk VID / PID and a generated serial number.
    //
    // On one hand, this serial number should be unique because
    // there should be no multiple devices with the same PID/VID/SN
    // combination at any given moment of time. On the other hand,
    // UsbDk should re-use serial numbers of unplugged devices,
    // because we don't want Windows to cache one more driver
    // information entry each time we redirect of hide a device.
    //
    // This function implements a simple but efficient mechanism
    // for serial number generation for virtual UsbDk devices

    CLockedContext<CWdmSpinLock> Ctx(m_HiddenDevicesLock);

    bool NoSuchSN = false;

    for (ULONG MinSN = 0; !NoSuchSN; MinSN++)
    {
        NoSuchSN = m_HiddenDevices.ForEach([MinSN](CUsbDkFilterDevice *Filter)
                   { return (Filter->GetSerialNumber() != MinSN); });

        if (NoSuchSN)
        {
            FilterDevice.SetSerialNumber(MinSN);
        }
    }

    m_HiddenDevices.PushBack(&FilterDevice);
}

void CUsbDkControlDevice::UnregisterHiddenDevice(CUsbDkFilterDevice &FilterDevice)
{
    CLockedContext<CWdmSpinLock> Ctx(m_HiddenDevicesLock);
    m_HiddenDevices.Remove(&FilterDevice);
}

bool CUsbDkControlDevice::ShouldHideDevice(CUsbDkChildDevice &Device) const
{
    const USB_DEVICE_DESCRIPTOR &DevDescriptor = Device.DeviceDescriptor();
    auto Hide = false;
    ULONG classes = Device.ClassesBitMask();
    const auto &HideVisitor = [&DevDescriptor, &Hide](CUsbDkHideRule *Entry) -> bool
    {
        Entry->Dump(TRACE_LEVEL_VERBOSE);
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FILTERDEVICE,
            "checking old hide rules %X:%X", DevDescriptor.idVendor, DevDescriptor.idProduct);
        if (Entry->Match(DevDescriptor))
        {
            Hide = Entry->ShouldHide();
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FILTERDEVICE, "Match: hide = %d", Hide);
            return !Entry->ForceDecision();
        }

        return true;
    };

    const auto &HideVisitorExt = [&DevDescriptor, &Hide, classes](CUsbDkHideRule *Entry) -> bool
    {
        Entry->Dump(TRACE_LEVEL_VERBOSE);
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FILTERDEVICE,
            "checking ext hide rules %X:%X", DevDescriptor.idVendor, DevDescriptor.idProduct);
        if (Entry->Match(classes, DevDescriptor))
        {
            Hide = Entry->ShouldHide();
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FILTERDEVICE, "Match: hide = %d", Hide);
            return !Entry->ForceDecision();
        }

        return true;
    };

    const_cast<HideRulesSet*>(&m_HideRules)->ForEach(HideVisitor);
    const_cast<HideRulesSet*>(&m_PersistentHideRules)->ForEach(HideVisitor);

    if (!Hide)
    {
        const_cast<HideRulesSet*>(&m_ExtHideRules)->ForEach(HideVisitorExt);
        const_cast<HideRulesSet*>(&m_PersistentExtHideRules)->ForEach(HideVisitorExt);
    }

    return Hide;
}

bool CUsbDkControlDevice::ShouldHide(const USB_DK_DEVICE_ID &DevId)
{
    bool b = false;

    EnumUsbDevicesByID(DevId,
        [&b, this](CUsbDkChildDevice *Child) -> bool
    {
        b = ShouldHideDevice(*Child);
        return false;
    });

    return b;
}

/* Ideally we would like to use IoGetDeviceProperty(DevicePropertyInstallState) to see if there */
/* is a driver that will get installed, or IoOpenDeviceRegistryKey(Device) to look for a Device */
/* value to see if the Device has a Driver assigned, but unfortunately nothing like this */
/* is possible within the IRP_MN_QUERY_DEVICE_RELATIONS because the PDO isn't actually valid */
/* for doing very much at that point in time. Instead we create and maintain a list built */
/* outside the MN_QUERY_IRP that has the associated registry key path in it for a VidPid + PortHub. */
/* This isn't a perfect solution, since the registry state will lag when a device is plugged in */
/* for the first time. The error should be benign, as we double check for a driver before */
/* doing anything that assumes this is a RawFiltered device, and the error will be corrected */
/* the second time the device is plugged in. */
bool CUsbDkControlDevice::ShouldRawFiltDevice(CUsbDkChildDevice &Device, bool Is2ndCall)
{
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Checking against %S, %S Is2ndCall %d",Device.DeviceID(), Device.LocationID(), Is2ndCall);

    ULONG VidPid, PortHub;

    /* Format is "USB\VID_XXXX&PID_XXXX" */
    auto status = EightHexToInteger(Device.DeviceID(), 8, 17, &VidPid);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
            "%!FUNC! Failed to Convert DeviceID string into ULONG (status %!STATUS!)", status);
        return false;
    }
    
    /* Format is "Port_#XXXX.Hub_#XXXX" */
    status = EightHexToInteger(Device.LocationID(), 6, 16, &PortHub);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
            "%!FUNC! Failed to Convert Location string into ULONG (status %!STATUS!)", status);
        return false;
    }
            
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
        "%!FUNC! ChildDevice Ident = 0x%08x 0x%08x'",VidPid, PortHub);

    /* Make sure that the "has a function driver" Set is initialized */
    if (!m_FDriverInited)
    {
        ReloadHasDriverList();
    }

    /* See if this Device has an entry in the Set */
    CUsbDkFDriverRule *Entry = nullptr;
    bool hasentry = false;
    bool hasfdriv = true;        /* default to not adding RawFilter */
    const auto &FiltVisitor = [VidPid, PortHub, &Entry, &hasentry, &hasfdriv, this](CUsbDkFDriverRule *e) -> bool
    {
        if (e->Match(VidPid, PortHub))
        {
            Entry = e;

            /* Check if there is a Driver value */
            CRegKey regkey;
            
            auto status = regkey.Open(*e->KeyName());
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                    "%!FUNC! Failed to open Key '%wZ' registry key",e->KeyName());
                hasentry = false;            /* Do ReloadHasDriverList() */
                return false;                /* Terminate ForEach() */
            }

            hasentry = true;            /* Don't ReloadHasDriverList() */

            CStringHolder DriverNameHolder;
            status = DriverNameHolder.Attach(TEXT("Driver"));
            ASSERT(NT_SUCCESS(status));

            CWdmMemoryBuffer Buffer;
            status = regkey.QueryValueInfo(*DriverNameHolder, KeyValuePartialInformation, Buffer);
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
                    "%!FUNC! Failed to read value '%wZ' (status %!STATUS!)", DriverNameHolder, status);
                hasfdriv = false;
            } else {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
                    "%!FUNC! Was able to read value '%wZ' (status %!STATUS!)", DriverNameHolder, status);
                hasfdriv = true;
            }
            return false;            /* Terminate ForEach() */
        }
        return true;            /* Continue ForEach() */
    };
    const_cast<FDriverRulesSet*>(&m_FDriversRules)->ForEach(FiltVisitor);

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
        "%!FUNC! Check FDriverRulesSet returned hasentry %d, hasfdriv %d",hasentry, hasfdriv);

    /* If there's no Set entry, then try and create one and check again. */
    if (!hasentry) {

        if (!Is2ndCall)          /* Try re-creating the list */
        {
            ReloadHasDriverList();
    
            /* Check again */
            hasentry = false;
            hasfdriv = true;       /* default to not adding RawFilter. */

            const_cast<FDriverRulesSet*>(&m_FDriversRules)->ForEach(FiltVisitor);
        }

        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
            "%!FUNC! Check 2 FDriverRulesSet got Set count %d, hasentry %d, hasfdriv %d",
                                          m_FDriversRules.GetCount(),hasentry, hasfdriv);

        /* If this fails (!hasentry) then either our assumptions in creating the Set */
        /* are wrong (i.e. Microsoft have changed the Registry layout for Enum\USB, in which */
        /* case GetCount() will be 0), or this is the first time the device has been plugged */
        /* in to the port, in which case we don't have a way of knowing if it has a driver. */
        /* We default to adding a Raw Filter so that the common case of plugging in a device */
        /* with no driver works with UsbDk, and take the risk that this won't disturb any */
        /* device that has a driver, since we double check before actually intercepting any */
        /* IRPs to the device. In the unlikely event this is a problem, then re-plugging the */
        /* device or redirecting via UsbDk should fix it. */
        if (m_FDriversRules.GetCount() > 0 && !hasentry)
        {
            hasfdriv = false;

            TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
                "%!FUNC! Check 2 FDriverRulesSet has no information so defaulting hasfdriv %d",hasfdriv);
        }
    }

    /* If a Function Driver for a device is installed after UsbDk and after a device is first */
    /* plugged in, then an Enum/USB entry will have been created for it, and PnP will ignore */
    /* the new Function Driver and assume that the device will continue to be driven in Raw mode. */
    /* This will create difficulty for the user, who then has to uninstall the device manually */
    /* using Device Manager to make it work with its new Function Driver as well as UsbDk. We can */
    /* avoid this problem if we set CONFIGFLAG_REINSTALL in the Enum/USB ConfigFlags, so */
    /* that PnP checks for a Function Driver on a Raw device each time it is plugged in. */
    if (Is2ndCall && Entry != nullptr && hasentry && !hasfdriv)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                    "%!FUNC! Setting m_SetReinstall flag on Key '%wZ'",Entry->KeyName());
        status = Device.SetRawDeviceToReinstall(Entry->KeyName());
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                "%!FUNC! Failed to SetRawDeviceToReinstall (status %!STATUS!)",status);
        }
    }
    return !hasfdriv;        /* Add RawFilter if there is no function driver */
}

bool CUsbDkControlDevice::ShouldRawFilt(const USB_DK_DEVICE_ID &DevId)
{
    bool b = false;

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
        "%!FUNC! About to call ShouldRawFiltDevice()");

    EnumUsbDevicesByID(DevId,
        [&b, this](CUsbDkChildDevice *Child) -> bool
    {
        b = ShouldRawFiltDevice(*Child, true);
        return false;
    });

    return b;
}

bool CUsbDkControlDevice::EnumerateDevices(USB_DK_DEVICE_INFO *outBuff, size_t numberAllocatedDevices, size_t &numberExistingDevices)
{
    numberExistingDevices = 0;

    return UsbDevicesForEachIf(ConstTrue,
                               [&outBuff, numberAllocatedDevices, &numberExistingDevices](CUsbDkChildDevice *Child) -> bool
                               {
                                   if (numberExistingDevices == numberAllocatedDevices)
                                   {
                                       TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! FAILED! Number existing devices is more than allocated buffer!");
                                       return false;
                                   }

                                   UsbDkFillIDStruct(&outBuff->ID, Child->DeviceID(), Child->InstanceID());

                                   outBuff->FilterID = Child->ParentID();
                                   outBuff->Port = Child->Port();
                                   outBuff->Speed = Child->Speed();
                                   outBuff->DeviceDescriptor = Child->DeviceDescriptor();

                                   outBuff++;
                                   numberExistingDevices++;
                                   return true;
                               });
}

// EnumUsbDevicesByID runs over the list of USB devices looking for device by ID.
// For each device with matching ID Functor() is called.
// If Functor() returns false EnumUsbDevicesByID() interrupts the loop and exits immediately.
//
// Return values:
//     - false: the loop was interrupted,
//     - true: the loop went over all devices registered

template <typename TFunctor>
bool CUsbDkControlDevice::EnumUsbDevicesByID(const USB_DK_DEVICE_ID &ID, TFunctor Functor)
{
    return UsbDevicesForEachIf([&ID](CUsbDkChildDevice *c) { return c->Match(ID.DeviceID, ID.InstanceID); }, Functor);
}

bool CUsbDkControlDevice::UsbDeviceExists(const USB_DK_DEVICE_ID &ID)
{
    return !EnumUsbDevicesByID(ID, ConstFalse);
}

bool CUsbDkControlDevice::GetDeviceDescriptor(const USB_DK_DEVICE_ID &DeviceID,
                                              USB_DEVICE_DESCRIPTOR &Descriptor)
{
    return !EnumUsbDevicesByID(DeviceID,
                               [&Descriptor](CUsbDkChildDevice *Child) -> bool
                               {
                                   Descriptor = Child->DeviceDescriptor();
                                   return false;
                               });
}

PDEVICE_OBJECT CUsbDkControlDevice::GetPDOByDeviceID(const USB_DK_DEVICE_ID &DeviceID)
{
    PDEVICE_OBJECT PDO = nullptr;

    EnumUsbDevicesByID(DeviceID,
                       [&PDO](CUsbDkChildDevice *Child) -> bool
                       {
                           PDO = Child->PDO();
                           ObReferenceObject(PDO);
                           return false;
                       });

    if (PDO == nullptr)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! PDO was not found");
    }

    return PDO;
}

CUsbDkChildDevice *CUsbDkControlDevice::GetChildByDeviceID(const USB_DK_DEVICE_ID &DeviceID)
{
    CUsbDkChildDevice *child = nullptr;

    EnumUsbDevicesByID(DeviceID,
                       [&child](CUsbDkChildDevice *Child) -> bool
                       {
                           child = Child;
                           return false;
                       });

    if (child == nullptr)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Child was not found");
    }

    return child;
}

// EnumUsbDevicesByPDO runs over the list of USB devices looking for device by PDO.
// For each device with matching PDO Functor() is called.
// If Functor() returns false EnumUsbDevicesByPDO() interrupts the loop and exits immediately.
//
// Return values:
//     - false: the loop was interrupted,
//     - true: the loop went over all devices registered

template <typename TFunctor>
bool CUsbDkControlDevice::EnumUsbDevicesByPDO(const PDEVICE_OBJECT PDO, TFunctor Functor)
{
    return UsbDevicesForEachIf([PDO](CUsbDkChildDevice *c) { return c->PDO() == PDO; }, Functor);
}

CUsbDkChildDevice *CUsbDkControlDevice::GetChildByPDO(const PDEVICE_OBJECT PDO)
{
    CUsbDkChildDevice *child = nullptr;

    EnumUsbDevicesByPDO(PDO,
                       [&child](CUsbDkChildDevice *Child) -> bool
                       {
                           child = Child;
                           return false;
                       });

    if (child == nullptr)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Child was not found");
    }

    return child;
}

NTSTATUS CUsbDkControlDevice::ResetUsbDevice(const USB_DK_DEVICE_ID &DeviceID)
{
    CUsbDkChildDevice *Child = GetChildByDeviceID(DeviceID);
    if (Child == nullptr)
    {
        return STATUS_NO_SUCH_DEVICE;
    }

    PDEVICE_OBJECT PDO = Child->PDO();
    ObReferenceObject(PDO);
    CWdmUsbDeviceAccess pdoAccess(PDO);

    /* Some devices on some hubs won't reliably CYCLE unless they are */
    /* configured and then reset... */
    if (Child->IfReallyRaw() && Child->GetRawConfiguration() != 1) {
        SetUsbConfiguration(Child, 1);
        pdoAccess.Reset();
    }

    auto status = pdoAccess.Cycle();

    ObDereferenceObject(PDO);

    return status;
}

/* Get CONFIGURATION_DESCRIPTOR from cache */
NTSTATUS CUsbDkControlDevice::GetUsbDeviceConfigurationDescriptor(const USB_DK_DEVICE_ID &DeviceID,
                                                                  UCHAR DescriptorIndex,
                                                                  USB_CONFIGURATION_DESCRIPTOR &Descriptor,
                                                                  size_t Length)
{
    bool result = false;

    if (EnumUsbDevicesByID(DeviceID, [&result, DescriptorIndex, &Descriptor, Length](CUsbDkChildDevice *Child) -> bool
                                     {
                                        result = Child->ConfigurationDescriptor(DescriptorIndex, Descriptor, Length);
                                        return false;
                                     }))
    {
        return STATUS_NOT_FOUND;
    }

    return result ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_REQUEST;
}

/* Search Config Descriptor for a given interface */
USB_INTERFACE_DESCRIPTOR *
FindInterfaceDesc(USB_CONFIGURATION_DESCRIPTOR *config_desc,
                    unsigned int size, int interface_number, int altsetting)
{
    USB_COMMON_DESCRIPTOR *desc = (USB_COMMON_DESCRIPTOR *)config_desc;
    char *p = (char *)desc;
    USB_INTERFACE_DESCRIPTOR *if_desc = NULL;

    if (!config_desc || (size < config_desc->wTotalLength))
    {
        return NULL;
    }

    while (size != 0 && size >= desc->bLength)
    {
        if (desc->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE)
        {
            if_desc = (USB_INTERFACE_DESCRIPTOR *)desc;

            if ((if_desc->bInterfaceNumber == (UCHAR)interface_number)
                    && (if_desc->bAlternateSetting == (UCHAR)altsetting))
            {
                return if_desc;
            }
        }
        size -= desc->bLength;
        p    += desc->bLength;
        desc = (USB_COMMON_DESCRIPTOR *)p;
    }
    return NULL;
}

NTSTATUS CUsbDkControlDevice::SetUsbConfiguration(CUsbDkChildDevice *Child, UCHAR configuration)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
        "%!FUNC! about to set configuration %d", configuration);

    PDEVICE_OBJECT PDO = Child->PDO();

    NTSTATUS status = STATUS_SUCCESS;
    URB urb, *urb_ptr = NULL;
    USB_CONFIGURATION_DESCRIPTOR Descriptor = {}, *desc_ptr = NULL;
    USB_INTERFACE_DESCRIPTOR *interface_descriptor = NULL;
    USBD_INTERFACE_LIST_ENTRY *interfaces = NULL;
    int i, j, interface_number, desc_size;

    memset(&urb, 0, sizeof(URB));

    /* Unset the configuration */
    if (configuration == 0)
    {
        urb.UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
        urb.UrbHeader.Length = sizeof(struct _URB_SELECT_CONFIGURATION);

        status = UsbDkSendUrbSynchronously(PDO, urb);

        if (!NT_SUCCESS(status) || !USBD_SUCCESS(urb.UrbHeader.Status))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                "%!FUNC! setting configuration %d failed: status: %!STATUS!, urb-status: %!STATUS!",
                        configuration, status, urb.UrbHeader.Status);
            return status;
        }
        Child->SetRawConfiguration(configuration);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
        "%!FUNC! about to get configuration %d", configuration);

    /* Initial get configuration to discover total length */
    status = Child->ConfigurationDescriptor(configuration-1, Descriptor, sizeof(Descriptor));

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
        "%!FUNC! getting configuration descriptor failed with %!STATUS!", status);
        goto SetConfigurationDone;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
        "%!FUNC! got configuration descriptor type 0x%x, len %d, total length %d",
         Descriptor.bDescriptorType, Descriptor.bLength, Descriptor.wTotalLength);

    if (Descriptor.bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
        "%!FUNC! configuration is unexpected type");
        status = STATUS_INVALID_PARAMETER;
        goto SetConfigurationDone;
    }

    desc_size = Descriptor.wTotalLength;
    desc_ptr = static_cast<USB_CONFIGURATION_DESCRIPTOR *>(ExAllocatePool(USBDK_NON_PAGED_POOL,
        desc_size));

    /* Get whole configuration */
    status = Child->ConfigurationDescriptor(configuration-1, *desc_ptr, desc_size);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
        "%!FUNC! getting whole configuration descriptor failed with %!STATUS!", status);
        goto SetConfigurationDone;
    }
    desc_size = Descriptor.wTotalLength;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
        "%!FUNC! got whole configuration descriptor length %d, numInterfaces %d",
         desc_size, Descriptor.bNumInterfaces);

    interfaces = static_cast<USBD_INTERFACE_LIST_ENTRY *>(ExAllocatePool(USBDK_NON_PAGED_POOL,
        (Descriptor.bNumInterfaces + 1) * sizeof(USBD_INTERFACE_LIST_ENTRY)));

    if (interfaces == nullptr) {
        status = STATUS_NO_MEMORY;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
            "%!FUNC! interfaces memory allocation failed");
        goto SetConfigurationDone;
    }

    memset(interfaces, 0, (Descriptor.bNumInterfaces + 1) * sizeof(USBD_INTERFACE_LIST_ENTRY));

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
        "%!FUNC! parsing %d interfaces", Descriptor.bNumInterfaces);

    interface_number = 0;
    for (i = 0; i < Descriptor.bNumInterfaces; i++)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
            "%!FUNC! doing if ix %d, starting at interface %d", i, interface_number);

        for (j = interface_number; j < 32; j++)
        {
            interface_descriptor =
                FindInterfaceDesc(desc_ptr, desc_size, j, 0);
            if (interface_descriptor)
            {
                interface_number = ++j;
                break;
            }
        }

        if (!interface_descriptor)
        {
            status = STATUS_INVALID_PARAMETER;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                "%!FUNC! unable to find interface descriptor %d for index %d", j, i);
            goto SetConfigurationDone;
        }
        else
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                "%!FUNC! found interface %d\n", interface_descriptor->bInterfaceNumber);
            interfaces[i].InterfaceDescriptor = interface_descriptor;
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
        "%!FUNC! building set configuration request");

    urb_ptr = USBD_CreateConfigurationRequestEx(desc_ptr, interfaces);
    if (!urb_ptr)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
          "%!FUNC! memory allocation failed");
        status = STATUS_NO_MEMORY;
        goto SetConfigurationDone;
    }

    for (i = 0; i < Descriptor.bNumInterfaces; i++)
    {
        for (j = 0; j < (int)interfaces[i].Interface->NumberOfPipes; j++)
        {
            interfaces[i].Interface->Pipes[j].MaximumTransferSize = 0x10000;
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
           "%!FUNC! sending configuration request");

    status = UsbDkSendUrbSynchronously(PDO, *urb_ptr);

    if (!NT_SUCCESS(status) || !USBD_SUCCESS(urb.UrbHeader.Status))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
            "%!FUNC! setting configuration %d failed: status: %!STATUS!, urb-status: %!STATUS!",
                        configuration, status, urb.UrbHeader.Status);
        if (NT_SUCCESS(status)) status = urb.UrbHeader.Status;
        goto SetConfigurationDone;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
           "%!FUNC! configuration request sucess");

    Child->SetRawConfiguration(configuration);

SetConfigurationDone:
    if (desc_ptr)
        ExFreePool(desc_ptr);

    if (interfaces)
        ExFreePool(interfaces);

    if (urb_ptr)
        ExFreePool(urb_ptr);

    return status;
}

void CUsbDkControlDevice::ContextCleanup(_In_ WDFOBJECT DeviceObject)
{
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE, "%!FUNC! Entry");

    auto deviceContext = UsbDkControlGetContext(DeviceObject);
    delete deviceContext->UsbDkControl;
}

NTSTATUS CUsbDkControlDevice::Create(WDFDRIVER Driver)
{
    CUsbDkControlDeviceInit DeviceInit;

    auto status = DeviceInit.Create(Driver);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, USBDK_CONTROL_DEVICE_EXTENSION);
    attr.EvtCleanupCallback = ContextCleanup;

    status = CWdfControlDevice::Create(DeviceInit, attr);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    UsbDkControlGetContext(m_Device)->UsbDkControl = this;

    CObjHolder<CUsbDkHiderDevice> HiderDevice(new CUsbDkHiderDevice());
    if (!HiderDevice)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Hider device allocation failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = HiderDevice->Create(Driver);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    m_HiderDevice = HiderDevice.detach();

    return m_HiderDevice->Register();
}

NTSTATUS CUsbDkControlDevice::Register()
{
    DECLARE_CONST_UNICODE_STRING(ntDosDeviceName, USBDK_DOSDEVICE_NAME);
    auto status = CreateSymLink(ntDosDeviceName);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = m_DeviceQueue.Create(*this);
    if (NT_SUCCESS(status))
    {
        FinishInitializing();
        ReloadPersistentHideRules();
    }

    return status;
}

void CUsbDkControlDevice::IoInCallerContext(WDFDEVICE Device, WDFREQUEST Request)
{
    NTSTATUS status = STATUS_SUCCESS;

    CControlRequest CtrlRequest(Request);
    WDF_REQUEST_PARAMETERS Params;
    CtrlRequest.GetParameters(Params);

    if (Params.Type == WdfRequestTypeDeviceControl &&
        Params.Parameters.DeviceIoControl.IoControlCode == IOCTL_USBDK_ADD_REDIRECT)
    {
        auto Context = CtrlRequest.Context();

        status = UsbDkCreateCurrentProcessHandle(Context->CallerProcessHandle);

        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! UsbDkCreateCurrentProcessHandle failed, %!STATUS!", status);

            CtrlRequest.SetStatus(status);
            CtrlRequest.SetOutputDataLen(0);
            return;
        }
    }

    status = WdfDeviceEnqueueRequest(Device, CtrlRequest);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! WdfDeviceEnqueueRequest failed, %!STATUS!", status);

        CtrlRequest.SetStatus(status);
        CtrlRequest.SetOutputDataLen(0);
        return;
    }

    CtrlRequest.Detach();
}

bool CUsbDkControlDeviceQueue::FetchBuffersForAddRedirectRequest(CControlRequest &WdfRequest, PUSB_DK_DEVICE_ID &DeviceId, PULONG64 &RedirectorDevice)
{
    size_t DeviceIdLen;
    auto status = WdfRequest.FetchInputObject(DeviceId, &DeviceIdLen);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! FetchInputObject failed, %!STATUS!", status);
        WdfRequest.SetStatus(status);
        WdfRequest.SetOutputDataLen(0);
        return false;
    }

    if (DeviceIdLen != sizeof(USB_DK_DEVICE_ID))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Wrong request buffer size (%llu, expected %llu)",
                    DeviceIdLen, sizeof(USB_DK_DEVICE_ID));
        WdfRequest.SetStatus(STATUS_INVALID_BUFFER_SIZE);
        WdfRequest.SetOutputDataLen(0);
        return false;
    }

    size_t RedirectorDeviceLength;
    status = WdfRequest.FetchOutputObject(RedirectorDevice, &RedirectorDeviceLength);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Failed to fetch output buffer. %!STATUS!", status);
        WdfRequest.SetOutputDataLen(0);
        WdfRequest.SetStatus(status);
        return false;
    }

    if (RedirectorDeviceLength != sizeof(ULONG64))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Wrong request input buffer size (%llu, expected %llu)",
            RedirectorDeviceLength, sizeof(ULONG64));
        WdfRequest.SetStatus(STATUS_INVALID_BUFFER_SIZE);
        WdfRequest.SetOutputDataLen(0);
        return false;
    }

    return true;
}

CRefCountingHolder<CUsbDkControlDevice> *CUsbDkControlDevice::m_UsbDkControlDevice = nullptr;

CUsbDkControlDevice* CUsbDkControlDevice::Reference(WDFDRIVER Driver)
{
    if (!m_UsbDkControlDevice->InitialAddRef())
    {
        return m_UsbDkControlDevice->Get();
    }
    else
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE, "%!FUNC! creating control device");
    }

    CObjHolder<CUsbDkControlDevice> dev(new CUsbDkControlDevice());
    if (!dev)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! cannot allocate control device");
        m_UsbDkControlDevice->Release();
        return nullptr;
    }

    auto status = dev->Create(Driver);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! cannot create control device %!STATUS!", status);
        m_UsbDkControlDevice->Release();
        return nullptr;
    }

    *m_UsbDkControlDevice = dev.detach();

    status = (*m_UsbDkControlDevice)->Register();
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! cannot register control device %!STATUS!", status);
        m_UsbDkControlDevice->Release();
        return nullptr;
    }

    return *m_UsbDkControlDevice;
}

bool CUsbDkControlDevice::Allocate()
{
    ASSERT(m_UsbDkControlDevice == nullptr);

    m_UsbDkControlDevice =
        new CRefCountingHolder<CUsbDkControlDevice>([](CUsbDkControlDevice *Dev){ Dev->Delete(); });

    if (m_UsbDkControlDevice == nullptr)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE, "%!FUNC! Cannot allocate control device holder");
        return false;
    }

    return true;
}

void CUsbDkControlDevice::AddRedirectRollBack(const USB_DK_DEVICE_ID &DeviceId, bool WithReset)
{
    auto res = m_Redirections.Delete(&DeviceId);
    if (!res)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Roll-back failed.");
    }

    if (!WithReset)
    {
        return;
    }

    auto resetRes = ResetUsbDevice(DeviceId);
    if (!NT_SUCCESS(resetRes))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Roll-back reset failed. %!STATUS!", resetRes);
    }
}

NTSTATUS CUsbDkControlDevice::GetConfigurationDescriptor(const USB_DK_CONFIG_DESCRIPTOR_REQUEST &Request,
                                                         PUSB_CONFIGURATION_DESCRIPTOR Descriptor,
                                                         size_t *OutputBuffLen)
{
    auto status = GetUsbDeviceConfigurationDescriptor(Request.ID, static_cast<UCHAR>(Request.Index), *Descriptor, *OutputBuffLen);
    *OutputBuffLen = NT_SUCCESS(status) ? min(Descriptor->wTotalLength, *OutputBuffLen) : 0;
    return status;
}

NTSTATUS CUsbDkControlDevice::AddRedirect(const USB_DK_DEVICE_ID &DeviceId, HANDLE RequestorProcess, PHANDLE RedirectorDevice)
{
    CUsbDkRedirection *Redirection;
    auto addRes = AddRedirectionToSet(DeviceId, &Redirection);
    if (!NT_SUCCESS(addRes))
    {
        return addRes;
    }
    Redirection->AddRef();
    CObjHolder<CUsbDkRedirection, CRefCountingDeleter> dereferencer(Redirection);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE, "%!FUNC! Success. New redirections list:");
    m_Redirections.Dump();

    auto resetRes = ResetUsbDevice(DeviceId);
    if (!NT_SUCCESS(resetRes))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Reset after start redirection failed. %!STATUS!", resetRes);
        AddRedirectRollBack(DeviceId, false);
        return resetRes;
    }

    auto waitRes = Redirection->WaitForAttachment();
    if ((waitRes == STATUS_TIMEOUT) || !NT_SUCCESS(waitRes))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Wait for redirector attachment failed. %!STATUS!", waitRes);
        AddRedirectRollBack(DeviceId, true);
        return (waitRes == STATUS_TIMEOUT) ? STATUS_DEVICE_NOT_CONNECTED : waitRes;
    }

    auto status = Redirection->CreateRedirectorHandle(RequestorProcess, RedirectorDevice);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! CreateRedirectorHandle() failed. %!STATUS!", status);
        AddRedirectRollBack(DeviceId, true);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    return STATUS_SUCCESS;
}

NTSTATUS CUsbDkControlDevice::AddHideRuleToSet(const USB_DK_HIDE_RULE &UsbDkRule, HideRulesSet &Set)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE, "%!FUNC! entry");

    auto MatchAllMapper = [](ULONG64 Value) -> ULONG
    { return Value == USB_DK_HIDE_RULE_MATCH_ALL ? USBDK_REG_HIDE_RULE_MATCH_ALL
                                                 : static_cast<ULONG>(Value); };

    CObjHolder<CUsbDkHideRule> NewRule(new CUsbDkHideRule(UsbDkRule.Hide ? true : false,
                                                          MatchAllMapper(UsbDkRule.Class),
                                                          MatchAllMapper(UsbDkRule.VID),
                                                          MatchAllMapper(UsbDkRule.PID),
                                                          MatchAllMapper(UsbDkRule.BCD)));
    if (!NewRule)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Failed to allocate new rule");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if(!Set.Add(NewRule))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! failed. Hide rule already present.");
        return STATUS_OBJECT_NAME_COLLISION;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE, "%!FUNC! Current hide rules:");
    Set.Dump();

    NewRule.detach();
    return STATUS_SUCCESS;
}

void CUsbDkControlDevice::ClearHideRules()
{
    m_HideRules.Clear();
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE, "%!FUNC! All dynamic hide rules dropped.");
}

class CHideRulesRegKey final : public CRegKey
{
public:
    NTSTATUS Open()
    {
        auto DriverParamsRegPath = CDriverParamsRegistryPath::Get();
        if (DriverParamsRegPath->Length == 0)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                "%!FUNC! Driver parameters registry key path not available.");

            return STATUS_INVALID_DEVICE_STATE;
        }

        CStringHolder ParamsSubkey;
        auto status = ParamsSubkey.Attach(TEXT("\\") USBDK_HIDE_RULES_SUBKEY_NAME);
        ASSERT(NT_SUCCESS(status));

        CString HideRulesRegPath;
        status = HideRulesRegPath.Create(DriverParamsRegPath, ParamsSubkey);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                "%!FUNC! Failed to allocate path to hide rules registry key.");

            return status;
        }

        status = CRegKey::Open(*HideRulesRegPath);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                "%!FUNC! Failed to open hide rules registry key.");
        }

        return status;
    }
};

class CRegHideRule final : private CRegKey
{
public:
    NTSTATUS Open(const CRegKey &HideRulesRegKey, const UNICODE_STRING &Name)
    {
        return CRegKey::Open(HideRulesRegKey, Name);
    }

    NTSTATUS Read(USB_DK_HIDE_RULE &Rule)
    {
        auto status = ReadBoolValue(USBDK_HIDE_RULE_SHOULD_HIDE, Rule.Hide);
        if (!NT_SUCCESS(status))
        {
            return status;
        }

        DWORD32 val;
        status = ReadDwordValue(USBDK_HIDE_RULE_TYPE, val);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        Rule.Type = val;

        status = ReadDwordMaskValue(USBDK_HIDE_RULE_VID, Rule.VID);
        if (!NT_SUCCESS(status))
        {
            return status;
        }

        status = ReadDwordMaskValue(USBDK_HIDE_RULE_PID, Rule.PID);
        if (!NT_SUCCESS(status))
        {
            return status;
        }

        status = ReadDwordMaskValue(USBDK_HIDE_RULE_BCD, Rule.BCD);
        if (!NT_SUCCESS(status))
        {
            return status;
        }

        status = ReadDwordMaskValue(USBDK_HIDE_RULE_CLASS, Rule.Class);
        if (!NT_SUCCESS(status))
        {
            return status;
        }

        return STATUS_SUCCESS;
    }

private:
    NTSTATUS ReadDwordValue(PCWSTR ValueName, DWORD32 &Value)
    {
        CStringHolder ValueNameHolder;
        auto status = ValueNameHolder.Attach(ValueName);
        ASSERT(NT_SUCCESS(status));

        CWdmMemoryBuffer Buffer;

        status = QueryValueInfo(*ValueNameHolder, KeyValuePartialInformation, Buffer);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
                "%!FUNC! Failed to query value %wZ (status %!STATUS!)", ValueNameHolder, status);

            return status;
        }

        auto Info = reinterpret_cast<PKEY_VALUE_PARTIAL_INFORMATION>(Buffer.Ptr());
        if (Info->Type != REG_DWORD)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE,
                "%!FUNC! Wrong data type for value %wZ: %d", ValueNameHolder, Info->Type);

            return STATUS_DATA_ERROR;
        }

        if (Info->DataLength != sizeof(DWORD32))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE,
                "%!FUNC! Wrong data length for value %wZ: %lu", ValueNameHolder, Info->DataLength);

            return STATUS_DATA_ERROR;
        }

        Value = *reinterpret_cast<PDWORD32>(&Info->Data[0]);
        return STATUS_SUCCESS;
    }

    NTSTATUS ReadBoolValue(PCWSTR ValueName, ULONG64 &Value)
    {
        DWORD32 RawValue;
        auto status = ReadDwordValue(ValueName, RawValue);

        if (NT_SUCCESS(status))
        {
            Value = HideRuleBoolFromRegistry(RawValue);
        }

        return status;
    }

    NTSTATUS ReadDwordMaskValue(PCWSTR ValueName, ULONG64 &Value)
    {
        DWORD32 RawValue;
        auto status = ReadDwordValue(ValueName, RawValue);

        if (NT_SUCCESS(status))
        {
            Value = HideRuleUlongMaskFromRegistry(RawValue);
        }

        return status;
    }
};

NTSTATUS CUsbDkControlDevice::ReloadPersistentHideRules()
{
    m_PersistentHideRules.Clear();
    m_PersistentExtHideRules.Clear();

    CHideRulesRegKey RulesKey;
    auto status = RulesKey.Open();
    if (NT_SUCCESS(status))
    {
        status = RulesKey.ForEachSubKey([&RulesKey, this](PCUNICODE_STRING Name)
        {
            CRegHideRule Rule;
            USB_DK_HIDE_RULE ParsedRule;

            if (NT_SUCCESS(Rule.Open(RulesKey, *Name)) &&
                NT_SUCCESS(Rule.Read(ParsedRule)))
            {
                AddPersistentHideRule(ParsedRule);
            }
        });
    }

    if (status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        status = STATUS_SUCCESS;
    }

    return status;
}

NTSTATUS CUsbDkControlDevice::AddRedirectionToSet(const USB_DK_DEVICE_ID &DeviceId, CUsbDkRedirection **NewRedirection)
{
    CObjHolder<CUsbDkRedirection, CRefCountingDeleter> newRedir(new CUsbDkRedirection());

    if (!newRedir)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! failed. Cannot allocate redirection.");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    auto status = newRedir->Create(DeviceId);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! failed. Cannot create redirection.");
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE, "%!FUNC! Adding new redirection");
    newRedir->Dump();

    if (!UsbDeviceExists(DeviceId))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! failed. Cannot redirect unknown device.");
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if (!m_Redirections.Add(newRedir))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! failed. Device already redirected.");
        return STATUS_OBJECT_NAME_COLLISION;
    }

    *NewRedirection = newRedir.detach();

    return STATUS_SUCCESS;
}

NTSTATUS CUsbDkControlDevice::RemoveRedirect(const USB_DK_DEVICE_ID &DeviceId, ULONG pid)
{
    if (NotifyRedirectorRemovalStarted(DeviceId, pid))
    {
        auto res = ResetUsbDevice(DeviceId);
        if (NT_SUCCESS(res))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                        "%!FUNC! Waiting for detachment from %S:%S",
                        DeviceId.DeviceID, DeviceId.InstanceID);

            if (!WaitForDetachment(DeviceId))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Wait for redirector detachment failed.");
                return STATUS_DEVICE_NOT_CONNECTED;
            }

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                        "%!FUNC! Detached from %S:%S",
                        DeviceId.DeviceID, DeviceId.InstanceID);
        }
        else if (res != STATUS_NO_SUCH_DEVICE)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! Usb device reset failed.");
            return res;
        }

        if (!m_Redirections.Delete(&DeviceId))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, "%!FUNC! No such redirection registered.");
            res = STATUS_OBJECT_NAME_NOT_FOUND;
        }

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE, "%!FUNC! Finished successfully. New redirections list:");
        m_Redirections.Dump();

        return STATUS_SUCCESS;
    }

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE,
                "%!FUNC! failed for %S:%S",
                DeviceId.DeviceID, DeviceId.InstanceID);

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

bool CUsbDkControlDevice::NotifyRedirectorAttached(CRegText *DeviceID, CRegText *InstanceID, CUsbDkFilterDevice *RedirectorDevice)
{
    USB_DK_DEVICE_ID ID;
    UsbDkFillIDStruct(&ID, *DeviceID->begin(), *InstanceID->begin());

    return m_Redirections.ModifyOne(&ID, [RedirectorDevice](CUsbDkRedirection *R){ R->NotifyRedirectorCreated(RedirectorDevice); });
}

bool CUsbDkControlDevice::NotifyRedirectorRemovalStarted(const USB_DK_DEVICE_ID &ID, ULONG pid)
{
    return m_Redirections.ModifyOne(&ID, [](CUsbDkRedirection *R){ R->NotifyRedirectionRemovalStarted(); }, pid);
}

bool CUsbDkControlDevice::WaitForDetachment(const USB_DK_DEVICE_ID &ID)
{
    CUsbDkRedirection *Redirection;
    auto res = m_Redirections.ModifyOne(&ID, [&Redirection](CUsbDkRedirection *R)
                                            { R->AddRef();
                                              Redirection = R;});
    if (!res)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! object was not found.");
        return res;
    }
    res = Redirection->WaitForDetachment();
    Redirection->Release();

    return res;
}

NTSTATUS CUsbDkRedirection::Create(const USB_DK_DEVICE_ID &Id)
{
    auto status = m_DeviceID.Create(Id.DeviceID);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    return m_InstanceID.Create(Id.InstanceID);
}

void CUsbDkRedirection::Dump(LPCSTR message) const
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                "%!FUNC! %s DevID: %wZ, InstanceID: %wZ",
                message,
                m_DeviceID, m_InstanceID);
}

bool CUsbDkRedirection::MatchProcess(ULONG pid)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_WDFDEVICE, "%!FUNC! pid 0x%X, owner 0x%X", pid, m_OwnerPid);
    return pid == m_OwnerPid;
}

void CUsbDkRedirection::NotifyRedirectorCreated(CUsbDkFilterDevice *RedirectorDevice)
{
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! Redirector created for");
    Dump();

    m_RedirectorDevice = RedirectorDevice;
    m_RedirectorDevice->AddRef();
    m_RedirectionCreated.Set();
}

void CUsbDkRedirection::NotifyRedirectionRemoved()
{
    if (IsPreparedForRemove())
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! Raising redirection removal event for");
        Dump();

        m_RedirectionRemoved.Set();
    }
}

void CUsbDkRedirection::NotifyRedirectionRemovalStarted()
{
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! Redirector removal started for");
    Dump();

    m_RemovalInProgress = true;
    m_RedirectorDevice->Release();
    m_RedirectorDevice = nullptr;
    m_RedirectionCreated.Clear();
}

bool CUsbDkRedirection::WaitForDetachment()
{
    auto waitRes = m_RedirectionRemoved.Wait(true, -SecondsTo100Nanoseconds(120));
    if ((waitRes == STATUS_TIMEOUT) || !NT_SUCCESS(waitRes))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! Wait of RedirectionRemoved event failed. %!STATUS!", waitRes);
        return false;
    }

    return true;
}

bool CUsbDkRedirection::operator==(const USB_DK_DEVICE_ID &Id) const
{
    return (m_DeviceID == Id.DeviceID) &&
           (m_InstanceID == Id.InstanceID);
}

bool CUsbDkRedirection::operator==(const CUsbDkChildDevice &Dev) const
{
    return (m_DeviceID == Dev.DeviceID()) &&
           (m_InstanceID == Dev.InstanceID());
}

bool CUsbDkRedirection::operator==(const CUsbDkRedirection &Other) const
{
    return (m_DeviceID == Other.m_DeviceID) &&
           (m_InstanceID == Other.m_InstanceID);
}

NTSTATUS CUsbDkRedirection::CreateRedirectorHandle(HANDLE RequestorProcess, PHANDLE ObjectHandle)
{
    // Although we got notification from devices enumeration thread regarding redirector creation
    // system requires some (rather short) time to get the device ready for requests processing.
    // In case of unfortunate timing we may try to open newly created redirector device before
    // it is ready, in this case we will get "no such device" error.
    // Unfortunately it looks like there is no way to ensure device is ready but spin around
    // and poll it for some time.

    static const LONGLONG RETRY_TIMEOUT_MS = 20;
    unsigned int iterationsLeft = 10000 / RETRY_TIMEOUT_MS; //Max timeout is 10 seconds

    NTSTATUS status;
    LARGE_INTEGER interval;
    interval.QuadPart = -MillisecondsTo100Nanoseconds(RETRY_TIMEOUT_MS);

    do
    {
        if (IsPreparedForRemove())
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_WDFDEVICE, "%!FUNC!: device already marked for removal");
            status = STATUS_DEVICE_REMOVED;
            break;
        }
        status = m_RedirectorDevice->CreateUserModeHandle(RequestorProcess, ObjectHandle);
        if (NT_SUCCESS(status))
        {
            ULONG pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_WDFDEVICE, "%!FUNC! done for process 0x%X", pid);
            m_OwnerPid = pid;
            return status;
        }

        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    } while (iterationsLeft-- > 0);

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! failed, %!STATUS!", status);
    return status;
}

LONG CUsbDkHideRule::m_defaultDumpLevel = TRACE_LEVEL_INFORMATION;
void CUsbDkHideRule::Dump(LONG traceLevel) const
{
    TraceEvents(traceLevel, TRACE_CONTROLDEVICE, "%!FUNC! Hide: %!bool!, C: %08X, V: %08X, P: %08X, BCD: %08X",
                m_Hide, m_Class, m_VID, m_PID, m_BCD);
}

void CDriverParamsRegistryPath::CreateFrom(PCUNICODE_STRING DriverRegPath)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
        "%!FUNC! Driver registry path: %wZ", DriverRegPath);

    m_Path = new CAllocatablePath;
    if (m_Path == nullptr)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE,
            "%!FUNC! Failed to allocate storage for parameters registry path");
        return;
    }

    CStringHolder ParamsSubkey;
    auto status = ParamsSubkey.Attach(TEXT("\\") USBDK_PARAMS_SUBKEY_NAME);
    ASSERT(NT_SUCCESS(status));

    status = m_Path->Create(DriverRegPath, ParamsSubkey);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE,
            "%!FUNC! Failed to duplicate parameters registry path");
    }
}

void CDriverParamsRegistryPath::Destroy()
{
    delete m_Path;
}

CDriverParamsRegistryPath::CAllocatablePath *CDriverParamsRegistryPath::m_Path = nullptr;

NTSTATUS CUsbDkControlDevice::AddHideRule(const USB_DK_HIDE_RULE &UsbDkRule)
{
    if (UsbDkRule.Type == USBDK_HIDER_RULE_DEFAULT)
    {
        return AddHideRuleToSet(UsbDkRule, m_HideRules);
    }
    else if (UsbDkRule.Type == USBDK_HIDER_RULE_DETERMINATIVE_TYPES)
    {
        return AddHideRuleToSet(UsbDkRule, m_ExtHideRules);
    }
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS CUsbDkControlDevice::AddPersistentHideRule(const USB_DK_HIDE_RULE &UsbDkRule)
{
    if (UsbDkRule.Type == USBDK_HIDER_RULE_DEFAULT)
    {
        return AddHideRuleToSet(UsbDkRule, m_PersistentHideRules);
    }
    else if (UsbDkRule.Type == USBDK_HIDER_RULE_DETERMINATIVE_TYPES)
    {
        return AddHideRuleToSet(UsbDkRule, m_PersistentExtHideRules);
    }
    return STATUS_INVALID_PARAMETER;
}

/* Create or re-create the "Has Function Driver" registry key Set */
NTSTATUS CUsbDkControlDevice::ReloadHasDriverList()
{
    /* See if we need to figure out the Registry root of ...\Enum\USB */
    if (!m_FDriverInited)
    {
        /* Find the first filter */
        CUsbDkFilterDevice *ffilter = nullptr;
        m_FilterDevices.ForEach([&ffilter](CUsbDkFilterDevice *Filter)
            {
                ffilter = Filter;
                return false;
            });
    
        if (ffilter == nullptr) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! No filters");
            STATUS_SUCCESS;        /* Ignore error ? */
        }
    
        /* Get the Hub Driver HW key */
        WDFKEY hwkeyh;
        auto status = WdfDeviceOpenRegistryKey(ffilter->WdfObject(), PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                               WDF_NO_OBJECT_ATTRIBUTES, &hwkeyh);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! Failed to get Control HWKey %!STATUS!",status);
            return status;
        }
    
        CRegKey regkey;
        regkey.Acquire(WdfRegistryWdmGetHandle(hwkeyh));
    
        // Could also use ObReferenceObjectByHandle(), ObQueryObjectName(), ObDereferenceObject()
        CWdmMemoryBuffer InfoBuffer;
        status = regkey.QueryKeyInfo(KeyNameInformation, InfoBuffer);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! Failed to get Key path %!STATUS!",status);
            WdfRegistryClose(hwkeyh);
            return status;
        }
    
        auto NameInfo = reinterpret_cast<PKEY_NAME_INFORMATION>(InfoBuffer.Ptr());
        CStringHolder RootHolder;
        RootHolder.Attach(NameInfo->Name, static_cast<USHORT>(NameInfo->NameLength));
    
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! Got Hub HW reg path '%wZ'",RootHolder);
    
        WdfRegistryClose(hwkeyh);
    
        /* Now we truncate m_RootName at the end of "\Enum\USB\" */
        if (!RootHolder.TruncateAfter(TEXT("\\Enum\\USB\\"))) {
            return STATUS_REGISTRY_IO_FAILED;        /* Hmm. */
        }
        
        m_RootName.Create(RootHolder);
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! Got Base HW reg path '%wZ'",
                                                                                  m_RootName);
        m_FDriverInited = true;
    }

    /* Open our root key */
    CRegKey rootkey;
    auto status2 = rootkey.Open(*m_RootName);
    if (!NT_SUCCESS(status2))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
            "%!FUNC! Failed to open RootKey '%wZ' registry key",m_RootName);
        return status2;
    }

    FDriverRulesSet Set;

    CStringHolder LocationNameHolder;
    auto status = LocationNameHolder.Attach(TEXT("LocationInformation"));
    ASSERT(NT_SUCCESS(status));

    /* Search the sub keys for "VID_????&PID_????" */
    status = rootkey.ForEachSubKey([rootkey, &LocationNameHolder, &Set, this]
                                                    (CStringHolder &Sub1Name)
    {
        if (!Sub1Name.WCMatch(TEXT("VID_????&PID_????")))
            return;

        //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
        //   "%!FUNC! Found matching Sub1Name '%wZ'",Sub1Name);

        /* Open the sub-key */
        CRegKey sub1key;
        auto status = sub1key.Open(rootkey, *Sub1Name);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                "%!FUNC! Failed to open Sub1Key '%wZ' %!STATUS!",Sub1Name,status);
            return;
        }

        /* Search the instance sub keys */
        status = sub1key.ForEachSubKey([sub1key, &Sub1Name, &LocationNameHolder, &Set, this]
                                                                   (CStringHolder &Sub2Name)
        {
            //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
            //   "%!FUNC! Searching instance '%wZ'",Sub2Name);

            /* Open the instance sub-key */
            CRegKey sub2key;
            auto status = sub2key.Open(sub1key, *Sub2Name);
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
                    "%!FUNC! Failed to open instance '%wZ' %!STATUS!",Sub2Name,status);
                return;
            }

            /* Get the a 'LocationInformation' value */
            CWdmMemoryBuffer Buffer;
            status = sub2key.QueryValueInfo(*LocationNameHolder, KeyValuePartialInformation, Buffer);
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
                    "%!FUNC! Failed to read value %wZ (status %!STATUS!)", LocationNameHolder, status);
                return;
            }
            //TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
            //    "%!FUNC! Found subkey 'LocationInfo'");

            auto Info = reinterpret_cast<PKEY_VALUE_PARTIAL_INFORMATION>(Buffer.Ptr());

            //TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
            //        "%!FUNC! Info Type = %d, Length = %d", Info->Type,Info->DataLength);
            if (Info->Type != REG_SZ
             || Info->DataLength > (21 * sizeof(WCHAR)))
                return;

            CStringHolder LocationValueHolder;
            status = LocationValueHolder.Attach(reinterpret_cast<PCWSTR>(&Info->Data[0]),
                                                     static_cast<USHORT>(Info->DataLength));
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
                    "%!FUNC! Failed to Attach to Location value (status %!STATUS!)", status);
                return;
            }
            
            //TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
            //    "%!FUNC! Location = '%wZ'",LocationValueHolder);

            if (!LocationValueHolder.WCMatch(TEXT("Port_#????.Hub_#????")))
                return;

            TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
                "%!FUNC! Function Driver Ident = %wZ %wZ'",Sub1Name, LocationValueHolder);

            /* Form the overall device sub-key name */
            CString KeyName;
            status = KeyName.Append(m_RootName);
            if (NT_SUCCESS(status)) status = KeyName.Append(Sub1Name);
            if (NT_SUCCESS(status)) status = KeyName.Append(TEXT("\\"));
            if (NT_SUCCESS(status)) status = KeyName.Append(Sub2Name);
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
                    "%!FUNC! Failed to create overall sub-key name (status %!STATUS!)", status);
                return;
            }
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_WDFDEVICE, "%!FUNC! Overall sub-key name '%wZ'",
                                                                                        KeyName);

            /* Get the device ID as integers */
            ULONG VidPid, PortHub;

            status = EightHexToInteger(Sub1Name, 4, 13, &VidPid);
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
                    "%!FUNC! Failed to Convert VidPid string into ULONG (status %!STATUS!)", status);
                return;
            }
            
            status = EightHexToInteger(LocationValueHolder, 6, 16, &PortHub);
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
                    "%!FUNC! Failed to Convert PortHub string into ULONG (status %!STATUS!)", status);
                return;
            }
            
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
                "%!FUNC! Function Driver Ident = 0x%08x 0x%08x'",VidPid, PortHub);

            /* (Note KeyName will be empty after the constructor.) */
            CObjHolder<CUsbDkFDriverRule> NewRule(new CUsbDkFDriverRule(VidPid, PortHub, KeyName));
            if (!NewRule)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE,
                     "%!FUNC! Failed to allocate new FDriver rule");
                return;
            }

            if(!Set.Add(NewRule))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE,
                   "%!FUNC! failed. FDriver rule already present.");
                return;
            }

            NewRule.detach();
        });

        if (status == STATUS_OBJECT_NAME_NOT_FOUND)
           status = STATUS_SUCCESS;
    });

    if (status == STATUS_OBJECT_NAME_NOT_FOUND)
        status = STATUS_SUCCESS;

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CONTROLDEVICE,
            "%!FUNC! Failed to open Sub1Keys of '%wZ' registry key",PCUNICODE_STRING(m_RootName));
        return status;
    }

    /* Overwrite m_FDriversRules with the temporary set */
    Set.MoveList(m_FDriversRules);

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_CONTROLDEVICE, 
         "%!FUNC! We now have %d entries in DFDriversRules",m_FDriversRules.GetCount());

    return STATUS_SUCCESS;
}

