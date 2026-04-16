// XboxOneWirelessDriver.h
// macOS DriverKit extension for Xbox One USB Wireless Adapter
// Targets macOS 12+ on Apple Silicon (M1/M2/M3/M4)
//
// Build requirements:
//   - Xcode 14+
//   - macOS SDK 12.0+
//   - Entitlement: com.apple.developer.driverkit.transport.usb
//   - Entitlement: com.apple.developer.driverkit.family.hid.device
//   - Entitlement: com.apple.developer.driverkit.family.hid.eventservice

#pragma once

#include <DriverKit/IOService.iig>
#include <DriverKit/IOUserClient.iig>
#include <USBDriverKit/IOUSBHostDevice.iig>
#include <USBDriverKit/IOUSBHostInterface.iig>
#include <USBDriverKit/IOUSBHostPipe.iig>
#include <HIDDriverKit/IOUserHIDDevice.iig>
#include <DriverKit/OSAction.iig>
#include <DriverKit/IODispatchQueue.iig>
#include <DriverKit/IOTimerDispatchSource.iig>
#include <DriverKit/IOBufferMemoryDescriptor.iig>

// Maximum number of simultaneously connected controllers
#define MAX_CONTROLLERS 8

// USB bulk transfer buffer sizes
#define USB_IN_BUFFER_SIZE  64
#define USB_OUT_BUFFER_SIZE 64

// Polling interval for USB IN pipe (milliseconds)
#define USB_POLL_INTERVAL_MS 4

// ──────────────────────────────────────────────
// Controller slot state
// ──────────────────────────────────────────────
struct ControllerSlot {
    bool     connected;
    uint8_t  clientId;
    uint8_t  sequence;      // outgoing sequence counter
    bool     initialized;   // initialization handshake complete
    bool     guidePressed;
};

// ──────────────────────────────────────────────
// Main driver class
// ──────────────────────────────────────────────
class XboxOneWirelessDriver : public IOService
{
public:
    virtual kern_return_t Start(IOService *provider) override;
    virtual kern_return_t Stop(IOService *provider) override;
    virtual void          free() override;

    // Called when USB data arrives on the IN pipe
    virtual void HandleIncomingUSBData(OSAction *action,
                                       IOReturn status,
                                       uint32_t actualByteCount,
                                       uint64_t completionTimestamp) TYPE(IOUSBHostPipe::CompleteAsyncIO);

private:
    // Setup helpers
    kern_return_t SetupUSBInterface();
    kern_return_t SetupBulkPipes();
    kern_return_t SendDongleInit();

    // Async read loop
    kern_return_t ScheduleNextRead();

    // Protocol handling
    void ProcessGIPFrame(const uint8_t *buf, uint32_t len);
    void HandleControllerAnnounce(uint8_t clientId);
    void HandleControllerInput(uint8_t clientId, const uint8_t *payload, uint8_t payloadLen);
    void HandleGuideButton(uint8_t clientId, bool pressed);
    void HandleControllerDisconnect(uint8_t clientId);
    void SendAcknowledge(uint8_t clientId, uint8_t command, uint8_t sequence);
    kern_return_t SendPacket(const uint8_t *buf, size_t len);

    // HID output for a slot
    void PostHIDReport(uint8_t slotIndex);

    // Members
    IOUSBHostDevice     *mDevice          = nullptr;
    IOUSBHostInterface  *mInterface       = nullptr;
    IOUSBHostPipe       *mBulkInPipe      = nullptr;
    IOUSBHostPipe       *mBulkOutPipe     = nullptr;

    IODispatchQueue     *mQueue           = nullptr;

    // Read buffer (double-buffered)
    IOBufferMemoryDescriptor *mInBuffer[2] = {};
    uint32_t              mInBufferIndex  = 0;

    // One HID device per controller slot
    // (Each acts as an independent virtual gamepad)
    IOService            *mHIDDevices[MAX_CONTROLLERS] = {};

    ControllerSlot        mSlots[MAX_CONTROLLERS]      = {};

    // Outgoing sequence counter (host→dongle)
    uint8_t               mHostSequence = 0;

    // Action object for async USB completion callback
    OSAction             *mReadAction    = nullptr;
};

// ──────────────────────────────────────────────
// Virtual HID Device (one per connected controller)
// ──────────────────────────────────────────────
class XboxVirtualHIDDevice : public IOUserHIDDevice
{
public:
    virtual kern_return_t Start(IOService *provider) override;
    virtual kern_return_t Stop(IOService *provider) override;

    // IOUserHIDDevice overrides
    virtual kern_return_t newDeviceDescription(OSDictionaryPtr *outDescription) override;
    virtual kern_return_t newReportDescriptor(IOMemoryDescriptorPtr *outDescriptor) override;
    virtual kern_return_t handleReport(
        uint64_t timestamp,
        IOMemoryDescriptor *report,
        uint32_t reportLength,
        IOHIDReportType reportType,
        uint32_t options) override;

    // Post an input report (called by parent driver)
    kern_return_t PostInputReport(const void *reportData, size_t reportLength);

    // Slot identifier (0-7)
    uint8_t slotIndex = 0;

private:
    IODispatchQueue *mQueue = nullptr;
};
