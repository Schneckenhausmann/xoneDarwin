// XboxOneWirelessDriver.cpp
// DriverKit USB driver for the Xbox One Wireless USB Adapter (VID 0x045E)
// Supports PID 0x02E6 (v1) and 0x02FD (v2)
//
// Protocol reference:
//   https://github.com/medusalix/xow  (reverse engineering source)
//   https://github.com/dlundqvist/xone (Linux driver)

#include "XboxOneWirelessDriver.h"
#include "XboxGIPProtocol.h"

#include <DriverKit/DriverKit.h>
#include <DriverKit/OSCollections.h>
#include <USBDriverKit/USBDriverKit.h>
#include <HIDDriverKit/HIDDriverKit.h>

// ═══════════════════════════════════════════════════════════
// MARK: - XboxOneWirelessDriver lifecycle
// ═══════════════════════════════════════════════════════════

kern_return_t XboxOneWirelessDriver::Start(IOService *provider)
{
    kern_return_t ret = kIOReturnSuccess;

    os_log(OS_LOG_DEFAULT, "XboxOneWireless: Start");

    // Call super first
    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: super::Start failed 0x%08x", ret);
        return ret;
    }

    // Cast provider to USB device
    mDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (!mDevice) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: provider is not a USB device");
        return kIOReturnNoDevice;
    }
    mDevice->retain();

    // Create a serial dispatch queue for all driver work
    ret = IODispatchQueue::Create("com.xbox.wireless.queue", 0, 0, &mQueue);
    if (ret != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: queue creation failed 0x%08x", ret);
        goto fail;
    }

    // Open the USB device
    ret = mDevice->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: device Open failed 0x%08x", ret);
        goto fail;
    }

    // Find and open bulk interface + pipes
    ret = SetupUSBInterface();
    if (ret != kIOReturnSuccess) goto fail;

    ret = SetupBulkPipes();
    if (ret != kIOReturnSuccess) goto fail;

    // Allocate double-buffered read memory
    for (int i = 0; i < 2; i++) {
        ret = IOBufferMemoryDescriptor::Create(
            kIOMemoryDirectionIn, USB_IN_BUFFER_SIZE, 0, &mInBuffer[i]);
        if (ret != kIOReturnSuccess) {
            os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: buffer alloc failed 0x%08x", ret);
            goto fail;
        }
    }

    // Create OSAction for the async USB read callback
    ret = CreateActionHandleIncomingUSBData(sizeof(void *), &mReadAction);
    if (ret != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: OSAction create failed 0x%08x", ret);
        goto fail;
    }

    // Wake up the dongle
    ret = SendDongleInit();
    if (ret != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: dongle init failed 0x%08x", ret);
        goto fail;
    }

    // Start the read loop
    ret = ScheduleNextRead();
    if (ret != kIOReturnSuccess) goto fail;

    // Announce ourselves to IOKit
    RegisterService();

    os_log(OS_LOG_DEFAULT, "XboxOneWireless: driver started successfully");
    return kIOReturnSuccess;

fail:
    Stop(provider);
    return ret;
}

kern_return_t XboxOneWirelessDriver::Stop(IOService *provider)
{
    os_log(OS_LOG_DEFAULT, "XboxOneWireless: Stop");

    // Stop all virtual HID devices
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (mHIDDevices[i]) {
            mHIDDevices[i]->Stop(this);
            OSSafeReleaseNULL(mHIDDevices[i]);
        }
    }

    // Abort pending pipe I/O
    if (mBulkInPipe) {
        mBulkInPipe->_Abort(this, kIOReturnAborted, 0);
    }

    OSSafeReleaseNULL(mReadAction);
    OSSafeReleaseNULL(mBulkInPipe);
    OSSafeReleaseNULL(mBulkOutPipe);
    OSSafeReleaseNULL(mInterface);

    for (int i = 0; i < 2; i++) {
        OSSafeReleaseNULL(mInBuffer[i]);
    }

    if (mDevice) {
        mDevice->Close(this, 0);
        OSSafeReleaseNULL(mDevice);
    }

    OSSafeReleaseNULL(mQueue);

    return Stop(provider, SUPERDISPATCH);
}

void XboxOneWirelessDriver::free()
{
    free(SUPERDISPATCH);
}

// ═══════════════════════════════════════════════════════════
// MARK: - USB Interface / Pipe Setup
// ═══════════════════════════════════════════════════════════

kern_return_t XboxOneWirelessDriver::SetupUSBInterface()
{
    // The Xbox Wireless Adapter exposes interface 0 as the main bulk interface.
    // Configuration: 1, Interface: 0, Alternate: 0
    OSObject *iterator = nullptr;
    kern_return_t ret = mDevice->CreateInterfaceIterator(&iterator);
    if (ret != kIOReturnSuccess || !iterator) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: CreateInterfaceIterator failed");
        return kIOReturnNotFound;
    }

    IOUSBHostInterface *iface = nullptr;
    OSIterator *iter = OSDynamicCast(OSIterator, iterator);
    if (iter) {
        OSObject *obj;
        while ((obj = iter->getNextObject()) != nullptr) {
            IOUSBHostInterface *candidate = OSDynamicCast(IOUSBHostInterface, obj);
            if (!candidate) continue;

            const IOUSBInterfaceDescriptor *desc = candidate->interfaceDescriptor();
            if (!desc) continue;

            // Interface 0, class 0xFF (vendor-specific)
            if (desc->bInterfaceNumber == 0 &&
                desc->bInterfaceClass == kUSBVendorSpecificClass)
            {
                iface = candidate;
                iface->retain();
                break;
            }
        }
        OSSafeReleaseNULL(iter);
    }
    OSSafeReleaseNULL(iterator);

    if (!iface) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: bulk interface not found");
        return kIOReturnNotFound;
    }

    kern_return_t openRet = iface->Open(this, 0, nullptr);
    if (openRet != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: interface Open failed 0x%08x", openRet);
        OSSafeReleaseNULL(iface);
        return openRet;
    }

    mInterface = iface;
    return kIOReturnSuccess;
}

kern_return_t XboxOneWirelessDriver::SetupBulkPipes()
{
    if (!mInterface) return kIOReturnNoDevice;

    uint8_t inAddr  = 0;
    uint8_t outAddr = 0;

    // Walk endpoint descriptors to find bulk IN and OUT
    const IOUSBConfigurationDescriptor *config = nullptr;
    const IOUSBInterfaceDescriptor     *iface  = nullptr;
    const IOUSBEndpointDescriptor      *ep     = nullptr;

    config = mDevice->activeConfigurationDescriptor();
    if (!config) return kIOReturnNotFound;

    iface = mInterface->interfaceDescriptor();
    if (!iface) return kIOReturnNotFound;

    while ((ep = IOUSBGetNextEndpointDescriptor(config, iface, ep)) != nullptr) {
        if ((ep->bmAttributes & kUSBEndpointTransferTypeMask) != kUSBEndpointTransferTypeBulk)
            continue;

        if ((ep->bEndpointAddress & kUSBEndpointDirectionMask) == kUSBEndpointDirectionIn) {
            inAddr = ep->bEndpointAddress;
        } else {
            outAddr = ep->bEndpointAddress;
        }
    }

    if (!inAddr || !outAddr) {
        os_log_error(OS_LOG_DEFAULT,
            "XboxOneWireless: bulk endpoints not found (in=0x%02x out=0x%02x)", inAddr, outAddr);
        return kIOReturnNotFound;
    }

    kern_return_t ret;

    ret = mInterface->CopyPipe(inAddr, &mBulkInPipe);
    if (ret != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: CopyPipe(IN) failed 0x%08x", ret);
        return ret;
    }

    ret = mInterface->CopyPipe(outAddr, &mBulkOutPipe);
    if (ret != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: CopyPipe(OUT) failed 0x%08x", ret);
        return ret;
    }

    os_log(OS_LOG_DEFAULT, "XboxOneWireless: bulk IN=0x%02x OUT=0x%02x", inAddr, outAddr);
    return kIOReturnSuccess;
}

// ═══════════════════════════════════════════════════════════
// MARK: - Dongle Initialization
// ═══════════════════════════════════════════════════════════

kern_return_t XboxOneWirelessDriver::SendDongleInit()
{
    // The dongle requires this exact 5-byte wake-up packet before it
    // starts forwarding controller events.
    return SendPacket(kDongleInitPacket, sizeof(kDongleInitPacket));
}

// ═══════════════════════════════════════════════════════════
// MARK: - Async Read Loop
// ═══════════════════════════════════════════════════════════

kern_return_t XboxOneWirelessDriver::ScheduleNextRead()
{
    IOBufferMemoryDescriptor *buf = mInBuffer[mInBufferIndex & 1];
    mInBufferIndex++;

    kern_return_t ret = mBulkInPipe->AsyncIO(buf, USB_IN_BUFFER_SIZE, mReadAction, 0);
    if (ret != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "XboxOneWireless: AsyncIO failed 0x%08x", ret);
    }
    return ret;
}

// Called by DriverKit runtime on the mQueue when USB data arrives
void XboxOneWirelessDriver::HandleIncomingUSBData(
    OSAction  *action,
    IOReturn   status,
    uint32_t   actualByteCount,
    uint64_t   completionTimestamp)
{
    if (status == kIOReturnAborted) {
        // Driver is shutting down
        return;
    }

    if (status != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT,
            "XboxOneWireless: USB read error 0x%08x, retrying", status);
        ScheduleNextRead();
        return;
    }

    if (actualByteCount >= 4) {
        // Map current buffer to read from (previous index, already flipped above)
        IOBufferMemoryDescriptor *buf = mInBuffer[(mInBufferIndex - 1) & 1];
        uint64_t addr = 0;
        uint64_t len  = 0;
        buf->GetAddressRange(&addr, &len);
        ProcessGIPFrame(reinterpret_cast<const uint8_t *>(addr), actualByteCount);
    }

    // Re-arm for next packet
    ScheduleNextRead();
}

// ═══════════════════════════════════════════════════════════
// MARK: - GIP Protocol Processing
// ═══════════════════════════════════════════════════════════

void XboxOneWirelessDriver::ProcessGIPFrame(const uint8_t *buf, uint32_t len)
{
    if (len < 4) return;  // need at least the 4-byte GIP header

    const GIPHeader *hdr = reinterpret_cast<const GIPHeader *>(buf);
    const uint8_t   *payload    = buf + 4;
    const uint8_t    payloadLen = (len - 4 < hdr->length) ? (len - 4) : hdr->length;

    os_log_debug(OS_LOG_DEFAULT,
        "GIP cmd=0x%02x client=%d seq=%d len=%d",
        hdr->command, hdr->client, hdr->sequence, hdr->length);

    switch (hdr->command) {

        case DONGLE_CMD_CONTROLLER_ADDED:
            HandleControllerAnnounce(hdr->client);
            break;

        case DONGLE_CMD_CONTROLLER_LOST:
            HandleControllerDisconnect(hdr->client);
            break;

        case GIP_CMD_ANNOUNCE:
            // Controller sends its announce on connection; acknowledge it
            SendAcknowledge(hdr->client, GIP_CMD_ANNOUNCE, hdr->sequence);
            if (!mSlots[hdr->client].initialized) {
                HandleControllerAnnounce(hdr->client);
            }
            break;

        case GIP_CMD_STATUS:
            // Heartbeat / power status; acknowledge
            SendAcknowledge(hdr->client, GIP_CMD_STATUS, hdr->sequence);
            break;

        case GIP_CMD_INPUT:
            // Controller button/axis input report
            if (payloadLen >= sizeof(GIPInputReport)) {
                HandleControllerInput(hdr->client, payload, payloadLen);
            }
            break;

        case GIP_CMD_GUIDE_BTN:
            // Guide / Xbox button (separate command)
            if (payloadLen >= 1) {
                HandleGuideButton(hdr->client, payload[0] != 0);
            }
            SendAcknowledge(hdr->client, GIP_CMD_GUIDE_BTN, hdr->sequence);
            break;

        default:
            // Unknown or unhandled command — log and ignore
            os_log_debug(OS_LOG_DEFAULT,
                "GIP unhandled cmd=0x%02x", hdr->command);
            break;
    }
}

void XboxOneWirelessDriver::HandleControllerAnnounce(uint8_t clientId)
{
    if (clientId >= MAX_CONTROLLERS) return;
    ControllerSlot &slot = mSlots[clientId];

    if (slot.connected) return;  // already tracked

    os_log(OS_LOG_DEFAULT, "XboxOneWireless: controller connected on slot %d", clientId);

    slot.connected   = true;
    slot.clientId    = clientId;
    slot.sequence    = 0;
    slot.initialized = true;
    slot.guidePressed = false;

    // Create a virtual HID gamepad for this slot
    if (!mHIDDevices[clientId]) {
        // Allocate and start a new XboxVirtualHIDDevice
        XboxVirtualHIDDevice *hid = nullptr;
        kern_return_t ret = OSTypeAlloc(XboxVirtualHIDDevice, &hid);
        if (ret == kIOReturnSuccess && hid) {
            hid->slotIndex = clientId;
            ret = hid->init(nullptr);
            if (ret == kIOReturnSuccess) {
                ret = hid->attach(this);
                if (ret == kIOReturnSuccess) {
                    hid->Start(this);
                    mHIDDevices[clientId] = hid;
                    return;
                }
            }
            OSSafeReleaseNULL(hid);
        }
        os_log_error(OS_LOG_DEFAULT,
            "XboxOneWireless: failed to create HID device for slot %d (0x%08x)", clientId, ret);
    }
}

void XboxOneWirelessDriver::HandleControllerInput(
    uint8_t clientId, const uint8_t *payload, uint8_t payloadLen)
{
    if (clientId >= MAX_CONTROLLERS) return;
    if (!mSlots[clientId].connected) return;
    if (!mHIDDevices[clientId]) return;

    const GIPInputReport *gip = reinterpret_cast<const GIPInputReport *>(payload);
    XboxHIDReport hid = {};
    GIPToHID(gip, &hid);

    // Carry guide button state from separate guide command
    hid.btn_guide = mSlots[clientId].guidePressed ? 1 : 0;

    XboxVirtualHIDDevice *hidDev =
        OSDynamicCast(XboxVirtualHIDDevice, mHIDDevices[clientId]);
    if (hidDev) {
        hidDev->PostInputReport(&hid, sizeof(hid));
    }
}

void XboxOneWirelessDriver::HandleGuideButton(uint8_t clientId, bool pressed)
{
    if (clientId >= MAX_CONTROLLERS) return;
    mSlots[clientId].guidePressed = pressed;

    // Re-post the last HID report so the guide button state propagates
    PostHIDReport(clientId);
}

void XboxOneWirelessDriver::HandleControllerDisconnect(uint8_t clientId)
{
    if (clientId >= MAX_CONTROLLERS) return;

    os_log(OS_LOG_DEFAULT, "XboxOneWireless: controller disconnected on slot %d", clientId);

    mSlots[clientId] = {};  // zero-initialize

    if (mHIDDevices[clientId]) {
        mHIDDevices[clientId]->Stop(this);
        OSSafeReleaseNULL(mHIDDevices[clientId]);
    }
}

void XboxOneWirelessDriver::PostHIDReport(uint8_t slotIndex)
{
    // Called when guide button changes without a new input frame.
    // We post a zeroed report with just the guide bit set/cleared.
    if (slotIndex >= MAX_CONTROLLERS) return;
    XboxVirtualHIDDevice *hidDev =
        OSDynamicCast(XboxVirtualHIDDevice, mHIDDevices[slotIndex]);
    if (!hidDev) return;

    XboxHIDReport hid = {};
    hid.btn_guide = mSlots[slotIndex].guidePressed ? 1 : 0;
    hid.hat       = 8;  // neutral
    hidDev->PostInputReport(&hid, sizeof(hid));
}

// ═══════════════════════════════════════════════════════════
// MARK: - ACK / Output
// ═══════════════════════════════════════════════════════════

void XboxOneWirelessDriver::SendAcknowledge(
    uint8_t clientId, uint8_t command, uint8_t sequence)
{
    uint8_t ackPayload[] = { 0x00 };
    uint8_t pkt[16] = {};
    size_t pktLen = GIPBuildPacket(
        pkt, sizeof(pkt),
        GIP_CMD_ACKNOWLEDGE, clientId, mHostSequence++,
        ackPayload, sizeof(ackPayload));
    if (pktLen > 0) {
        SendPacket(pkt, pktLen);
    }
}

kern_return_t XboxOneWirelessDriver::SendPacket(const uint8_t *buf, size_t len)
{
    if (!mBulkOutPipe) return kIOReturnNoDevice;

    // Create a temporary memory descriptor for the output data
    IOBufferMemoryDescriptor *mem = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOut, len, 0, &mem);
    if (ret != kIOReturnSuccess || !mem) return ret;

    uint64_t addr = 0, bufLen = 0;
    mem->GetAddressRange(&addr, &bufLen);
    __builtin_memcpy(reinterpret_cast<void *>(addr), buf, len);

    uint32_t bytesTransferred = 0;
    ret = mBulkOutPipe->IO(mem, static_cast<uint32_t>(len), &bytesTransferred, 0);

    OSSafeReleaseNULL(mem);
    return ret;
}

// ═══════════════════════════════════════════════════════════
// MARK: - XboxVirtualHIDDevice
// ═══════════════════════════════════════════════════════════

kern_return_t XboxVirtualHIDDevice::Start(IOService *provider)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;

    ret = IODispatchQueue::Create("com.xbox.hid.queue", 0, 0, &mQueue);
    if (ret != kIOReturnSuccess) return ret;

    RegisterService();
    os_log(OS_LOG_DEFAULT, "XboxVirtualHID: slot %d started", slotIndex);
    return kIOReturnSuccess;
}

kern_return_t XboxVirtualHIDDevice::Stop(IOService *provider)
{
    os_log(OS_LOG_DEFAULT, "XboxVirtualHID: slot %d stopped", slotIndex);
    OSSafeReleaseNULL(mQueue);
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t XboxVirtualHIDDevice::newDeviceDescription(OSDictionaryPtr *outDescription)
{
    OSDictionary *dict = OSDictionary::withCapacity(8);
    if (!dict) return kIOReturnNoMemory;

    // Vendor/product info shown in System Information
    OSDictionarySetValue(dict, "VendorID",      OSNumber::withNumber(XBOX_VENDOR_ID, 32));
    OSDictionarySetValue(dict, "ProductID",     OSNumber::withNumber(XBOX_DONGLE_PID_V2, 32));
    OSDictionarySetValue(dict, "VersionNumber", OSNumber::withNumber(0x0100, 32));

    // Human-readable strings
    char name[64];
    __builtin_snprintf(name, sizeof(name), "Xbox Wireless Controller (Slot %d)", slotIndex + 1);
    OSDictionarySetValue(dict, "Product",       OSString::withCString(name));
    OSDictionarySetValue(dict, "Manufacturer",  OSString::withCString("Microsoft Corporation"));

    // HID usage: Generic Desktop / Gamepad
    OSDictionarySetValue(dict, "PrimaryUsage",     OSNumber::withNumber(0x05, 32));
    OSDictionarySetValue(dict, "PrimaryUsagePage", OSNumber::withNumber(0x01, 32));

    // Transport
    OSDictionarySetValue(dict, "Transport", OSString::withCString("USB"));

    *outDescription = dict;
    return kIOReturnSuccess;
}

kern_return_t XboxVirtualHIDDevice::newReportDescriptor(IOMemoryDescriptorPtr *outDescriptor)
{
    // Create a memory descriptor containing the HID report descriptor
    IOBufferMemoryDescriptor *mem = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOut,
        sizeof(kXboxHIDReportDescriptor),
        0, &mem);
    if (ret != kIOReturnSuccess) return ret;

    uint64_t addr = 0, len = 0;
    mem->GetAddressRange(&addr, &len);
    __builtin_memcpy(reinterpret_cast<void *>(addr),
                     kXboxHIDReportDescriptor,
                     sizeof(kXboxHIDReportDescriptor));

    *outDescriptor = mem;
    return kIOReturnSuccess;
}

kern_return_t XboxVirtualHIDDevice::handleReport(
    uint64_t            timestamp,
    IOMemoryDescriptor *report,
    uint32_t            reportLength,
    IOHIDReportType     reportType,
    uint32_t            options)
{
    // Output reports (rumble) would be received here from the OS/app.
    // For now, pass up to super.
    return handleReport(timestamp, report, reportLength, reportType, options, SUPERDISPATCH);
}

kern_return_t XboxVirtualHIDDevice::PostInputReport(
    const void *reportData, size_t reportLength)
{
    IOBufferMemoryDescriptor *mem = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOut, reportLength, 0, &mem);
    if (ret != kIOReturnSuccess) return ret;

    uint64_t addr = 0, len = 0;
    mem->GetAddressRange(&addr, &len);
    __builtin_memcpy(reinterpret_cast<void *>(addr), reportData, reportLength);

    uint64_t timestamp = 0;
    clock_get_uptime(&timestamp);

    ret = handleReport(timestamp, mem, static_cast<uint32_t>(reportLength),
                       kIOHIDReportTypeInput, 0);

    OSSafeReleaseNULL(mem);
    return ret;
}
