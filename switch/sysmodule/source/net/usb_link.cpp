#include "net/usb_link.hpp"

#include <algorithm>
#include <cstring>

#include "core/config.hpp"
#include "core/power.hpp"
#include "core/system.hpp"
#include "protocol.hpp"

namespace {
constexpr size_t kChunk = 0x10000;
constexpr size_t kReadSize = 0x1000;
constexpr size_t kRingSize = 0x2000;
constexpr int kSliceMs = 250;
constexpr int kDrainBudgetMs = 1500;
constexpr int kDrainSliceMs = 50;
constexpr u32 kUrbFinished = 3;
constexpr u32 kUrbInProgress = 2;
constexpr int kByeTimeoutMs = 200;
constexpr Result kTimedOut = MAKERESULT(Module_Kernel, KernelError_TimedOut);

alignas(0x1000) uint8_t g_write[kChunk];
alignas(0x1000) uint8_t g_read[kReadSize];
uint8_t g_ring[kRingSize];

const u8 kBos[] = {0x05, USB_DT_BOS, 0x16, 0x00, 0x02, 0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,
                   0x0A, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00};

const char* speedName(UsbDeviceSpeed s) {
    switch (s) {
        case UsbDeviceSpeed_Low: return "low";
        case UsbDeviceSpeed_Full: return "full";
        case UsbDeviceSpeed_High: return "high";
        case UsbDeviceSpeed_Super: return "super";
        default: return "unknown";
    }
}

uint32_t packetSize(UsbDeviceSpeed s) {
    switch (s) {
        case UsbDeviceSpeed_Low:
        case UsbDeviceSpeed_Full: return 0x40;
        case UsbDeviceSpeed_Super: return 0x400;
        default: return 0x200;
    }
}

bool waitEvent(Event* e, int ms, Result* rc) {
    *rc = eventWait(e, static_cast<u64>(ms) * 1000000ull);
    return R_SUCCEEDED(*rc);
}
}

void UsbChannel::fail(const char* what, Result rc) {
    LOG("usb: %s failed 0x%x", what, static_cast<unsigned>(rc));
}

bool UsbChannel::step(const char* what, Result rc) {
    if (R_SUCCEEDED(rc)) return true;
    if (++openFailures_ == 1 || openFailures_ % 15 == 0)
        LOG("usb: %s failed 0x%x (attempt %u)", what, static_cast<unsigned>(rc), static_cast<unsigned>(openFailures_));
    return false;
}

bool UsbChannel::open() {
    if (stuck_.load()) return false;
    if (open_.load()) return true;
    if (hosversionBefore(5, 0, 0)) {
        step("firmware check", MAKERESULT(Module_Libnx, LibnxError_NotFound));
        return false;
    }

    const Result init = usbDsInitialize();
    if (R_FAILED(init)) {
        if (++openFailures_ == 1 || openFailures_ % 15 == 0)
            LOG("usb: usb:ds unavailable 0x%x (attempt %u)", static_cast<unsigned>(init),
                static_cast<unsigned>(openFailures_));
        return false;
    }
    initialized_ = true;

    static const u16 langs[] = {0x0409};
    u8 iManufacturer = 0, iProduct = 0, iSerial = 0;
    usb_device_descriptor dev{};
    dev.bLength = USB_DT_DEVICE_SIZE;
    dev.bDescriptorType = USB_DT_DEVICE;
    dev.bcdUSB = 0x0110;
    dev.bMaxPacketSize0 = 0x40;
    dev.idVendor = proto::kUsbVendorId;
    dev.idProduct = proto::kUsbProductId;
    dev.bcdDevice = proto::kUsbBcdDevice;
    dev.bNumConfigurations = 1;

    usb_interface_descriptor ifd{};
    ifd.bLength = USB_DT_INTERFACE_SIZE;
    ifd.bDescriptorType = USB_DT_INTERFACE;
    ifd.bInterfaceNumber = USBDS_DEFAULT_InterfaceNumber;
    ifd.bNumEndpoints = 2;
    ifd.bInterfaceClass = USB_CLASS_VENDOR_SPEC;
    ifd.bInterfaceSubClass = 0xFF;
    ifd.bInterfaceProtocol = 0xFF;

    usb_ss_endpoint_companion_descriptor comp{};
    comp.bLength = USB_DT_SS_ENDPOINT_COMPANION_SIZE;
    comp.bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION;
    comp.bMaxBurst = 0x0F;

    bool ok = step("clear device data", usbDsClearDeviceData()) &&
              step("language descriptor", usbDsAddUsbLanguageStringDescriptor(nullptr, langs, 1)) &&
              step("manufacturer string", usbDsAddUsbStringDescriptor(&iManufacturer, proto::kUsbManufacturer)) &&
              step("product string", usbDsAddUsbStringDescriptor(&iProduct, proto::kUsbProduct)) &&
              step("serial string", usbDsAddUsbStringDescriptor(&iSerial, serial_.c_str()));
    if (ok) LOG("usb: serial string set (%u chars)", static_cast<unsigned>(serial_.size()));
    dev.iManufacturer = iManufacturer;
    dev.iProduct = iProduct;
    dev.iSerialNumber = iSerial;

    ok = ok && step("full speed device descriptor", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &dev));
    dev.bcdUSB = 0x0200;
    ok = ok && step("high speed device descriptor", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev));
    dev.bcdUSB = 0x0300;
    dev.bMaxPacketSize0 = 0x09;
    ok = ok && step("super speed device descriptor", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dev)) &&
         step("binary object store", usbDsSetBinaryObjectStore(kBos, sizeof kBos)) &&
         step("register interface", usbDsRegisterInterface(&iface_));

    if (ok) {
        ifd.bInterfaceNumber = iface_->interface_index;
        LOG("usb: interface %u", static_cast<unsigned>(iface_->interface_index));
    }

    const UsbDeviceSpeed speeds[] = {UsbDeviceSpeed_Full, UsbDeviceSpeed_High, UsbDeviceSpeed_Super};
    for (size_t i = 0; ok && i < 3; ++i) {
        usb_endpoint_descriptor epIn{};
        epIn.bLength = USB_DT_ENDPOINT_SIZE;
        epIn.bDescriptorType = USB_DT_ENDPOINT;
        epIn.bEndpointAddress = proto::kUsbEndpointIn;
        epIn.bmAttributes = USB_TRANSFER_TYPE_BULK;
        epIn.wMaxPacketSize = static_cast<uint16_t>(packetSize(speeds[i]));
        usb_endpoint_descriptor epOut = epIn;
        epOut.bEndpointAddress = proto::kUsbEndpointOut;
        const bool super = speeds[i] == UsbDeviceSpeed_Super;

        ok = step("append interface descriptor",
                  usbDsInterface_AppendConfigurationData(iface_, speeds[i], &ifd, USB_DT_INTERFACE_SIZE)) &&
             step("append in endpoint descriptor",
                  usbDsInterface_AppendConfigurationData(iface_, speeds[i], &epIn, USB_DT_ENDPOINT_SIZE)) &&
             (!super || step("append in companion descriptor",
                             usbDsInterface_AppendConfigurationData(iface_, speeds[i], &comp,
                                                                    USB_DT_SS_ENDPOINT_COMPANION_SIZE))) &&
             step("append out endpoint descriptor",
                  usbDsInterface_AppendConfigurationData(iface_, speeds[i], &epOut, USB_DT_ENDPOINT_SIZE)) &&
             (!super || step("append out companion descriptor",
                             usbDsInterface_AppendConfigurationData(iface_, speeds[i], &comp,
                                                                    USB_DT_SS_ENDPOINT_COMPANION_SIZE)));
    }

    ok = ok && step("register in endpoint", usbDsInterface_RegisterEndpoint(iface_, &in_, proto::kUsbEndpointIn)) &&
         step("register out endpoint", usbDsInterface_RegisterEndpoint(iface_, &out_, proto::kUsbEndpointOut)) &&
         step("enable interface", usbDsInterface_EnableInterface(iface_)) && step("enable device", usbDsEnable());

    if (!ok) {
        shutdown();
        return false;
    }

    inPending_ = false;
    outPending_ = false;
    kicked_ = false;
    strayReports_ = 0;
    ringHead_ = ringTail_ = 0;
    sentBytes_ = receivedBytes_ = 0;
    readErrors_ = stateFailures_ = 0;
    broken_ = false;
    lastState_ = -1;
    open_ = true;
    LOG("usb: usb:ds ready (vid 0x%04x pid 0x%04x), waiting for the host after %u failed attempts",
        static_cast<unsigned>(proto::kUsbVendorId), static_cast<unsigned>(proto::kUsbProductId),
        static_cast<unsigned>(openFailures_));
    openFailures_ = 0;
    return true;
}

void UsbChannel::releaseEndpoints() {
    if (in_) usbDsEndpoint_Close(in_);
    if (out_) usbDsEndpoint_Close(out_);
    if (iface_) usbDsInterface_Close(iface_);
    in_ = out_ = nullptr;
    iface_ = nullptr;
}

void UsbChannel::markStuck() {
    broken_ = true;
    open_ = false;
    if (!stuck_.exchange(true)) LOG("usb: transfer stuck, USB needs a console restart");
}

void UsbChannel::shutdown() {
    if (!initialized_ || stuck_.load()) return;
    if (open_.exchange(false))
        LOG("usb: shutting down usb:ds (%llu KB sent, %llu KB received)",
            static_cast<unsigned long long>(sentBytes_ / 1024), static_cast<unsigned long long>(receivedBytes_ / 1024));
    flush();
    if (stuck_.load()) return;
    if (iface_) usbDsInterface_DisableInterface(iface_);
    usbDsDisable();
    releaseEndpoints();
    usbDsExit();
    initialized_ = false;
    broken_ = false;
    lastState_ = -1;
}

bool UsbChannel::usable() const {
    return open_.load() && !broken_.load() && !abort_.load() && !kicked_.load() && !power::sleeping.load() &&
           !power::halted();
}

bool UsbChannel::idleStop() {
    return !open_.load() || kicked_.exchange(false) || power::sleeping.load() || power::halted() ||
           settings::transport.load() != settings::Transport::Usb;
}

bool UsbChannel::stopRequested() {
    return abort_.load() || kicked_.load() || idleStop();
}

bool UsbChannel::configured() {
    UsbState st = UsbState_Detached;
    const Result rc = usbDsGetState(&st);
    if (R_FAILED(rc)) {
        const uint32_t failures = stateFailures_.fetch_add(1) + 1;
        if (failures == 1 || failures % 40 == 0)
            LOG("usb: state query failed 0x%x (failure %u)", static_cast<unsigned>(rc),
                static_cast<unsigned>(failures));
        if (failures >= 40) broken_ = true;
        return false;
    }
    stateFailures_ = 0;
    const int prev = lastState_.exchange(static_cast<int>(st));
    if (prev != static_cast<int>(st)) {
        if (st == UsbState_Configured) {
            UsbDeviceSpeed speed = UsbDeviceSpeed_None;
            const Result src = usbDsGetSpeed(&speed);
            if (R_SUCCEEDED(src)) maxPacket_ = packetSize(speed);
            LOG("usb: configured by the host, speed %s (query 0x%x), max packet %u", speedName(speed),
                static_cast<unsigned>(src), static_cast<unsigned>(maxPacket_.load()));
        } else {
            LOG("usb: state %d%s", static_cast<int>(st), prev == UsbState_Configured ? " (unconfigured)" : "");
        }
    }
    return st == UsbState_Configured;
}

bool UsbChannel::waitConfigured(int sliceMs) {
    if (idleStop()) return false;
    if (configured()) return true;
    const uint64_t start = sys::nowMs();
    Result rc = 0;
    waitEvent(usbDsGetStateChangeEvent(), std::min(sliceMs, kSliceMs), &rc);
    eventClear(usbDsGetStateChangeEvent());
    if (sys::nowMs() - start < 20) sys::sleepMs(20);
    if (idleStop()) return false;
    return configured();
}

Result UsbChannel::postRead() {
    return usbDsEndpoint_PostBufferAsync(out_, g_read, kReadSize, &outUrb_);
}

UsbChannel::Pump UsbChannel::pump(int waitMs, bool session) {
    sys::Lock lock(outMutex_);
    Result rc = 0;
    if (session ? stopRequested() : idleStop()) return Pump::Aborted;
    if (!outPending_) {
        if (kRingSize - (ringHead_ - ringTail_) < kReadSize) {
            LOG("usb: receive ring overflow, ending the session");
            broken_ = true;
            return Pump::Fatal;
        }
        rc = postRead();
        if (R_FAILED(rc)) {
            if (!configured()) return Pump::Lost;
            fail("post read", rc);
            broken_ = true;
            return Pump::Fatal;
        }
        outPending_ = true;
    }

    if (!waitEvent(&out_->CompletionEvent, std::min(waitMs, kSliceMs), &rc)) {
        if (rc != kTimedOut) {
            fail("wait read", rc);
            broken_ = true;
            return Pump::Fatal;
        }
        if (session ? stopRequested() : idleStop()) return Pump::Aborted;
        return configured() ? Pump::Timeout : Pump::Lost;
    }

    eventClear(&out_->CompletionEvent);
    Urb state = Urb::Pending;
    UsbDsReportEntry e{};
    rc = poll(out_, outUrb_, &state, &e);
    if (R_FAILED(rc)) {
        fail("read report", rc);
        broken_ = true;
        return Pump::Fatal;
    }
    if (state == Urb::Pending) return Pump::Timeout;
    outPending_ = false;
    if (state == Urb::Done) {
        readErrors_ = 0;
        const uint32_t n = std::min<uint32_t>(e.transferredSize, kReadSize);
        for (uint32_t k = 0; k < n; ++k) g_ring[(ringHead_ + k) % kRingSize] = g_read[k];
        ringHead_ += n;
        receivedBytes_ += n;
        return Pump::Ok;
    }
    if (session ? stopRequested() : idleStop()) return Pump::Aborted;
    if (!configured()) return Pump::Lost;
    if (++readErrors_ == 1 || readErrors_ % 30 == 0)
        LOG("usb: read urb %u ended with status %u after %u bytes (error %u)", static_cast<unsigned>(e.id),
            static_cast<unsigned>(e.urb_status), static_cast<unsigned>(e.transferredSize),
            static_cast<unsigned>(readErrors_));
    if (readErrors_ >= 5) {
        broken_ = true;
        return Pump::Fatal;
    }
    sys::sleepMs(50);
    return Pump::Ok;
}

bool UsbChannel::waitReadable(int sliceMs) {
    if (!open_.load() || broken_.load()) return false;
    if (ringHead_ != ringTail_) return true;
    const Pump p = pump(sliceMs, false);
    if (p == Pump::Lost) {
        flush();
        sys::sleepMs(50);
    }
    return ringHead_ != ringTail_;
}

net::IoStatus UsbChannel::recvExact(void* buf, size_t size, int idleTimeoutMs) {
    auto* p = static_cast<uint8_t*>(buf);
    uint64_t deadline = sys::nowMs() + static_cast<uint64_t>(idleTimeoutMs);
    while (size > 0) {
        if (ringHead_ != ringTail_) {
            const size_t n = std::min<size_t>(size, ringHead_ - ringTail_);
            for (size_t k = 0; k < n; ++k) p[k] = g_ring[(ringTail_ + k) % kRingSize];
            ringTail_ += static_cast<uint32_t>(n);
            p += n;
            size -= n;
            deadline = sys::nowMs() + static_cast<uint64_t>(idleTimeoutMs);
            continue;
        }
        if (abort_.load() || kicked_.load() || power::sleeping.load() || power::halted()) return net::IoStatus::Aborted;
        if (broken_.load()) return net::IoStatus::Error;
        switch (pump(kSliceMs, true)) {
            case Pump::Ok:
            case Pump::Timeout: break;
            case Pump::Aborted: return net::IoStatus::Aborted;
            case Pump::Lost: return net::IoStatus::Closed;
            case Pump::Fatal: return net::IoStatus::Error;
        }
        if (ringHead_ == ringTail_ && sys::nowMs() >= deadline) return net::IoStatus::Timeout;
    }
    if (usable()) {
        sys::Lock lock(outMutex_);
        if (!outPending_ && ringHead_ == ringTail_ && R_SUCCEEDED(postRead())) outPending_ = true;
    }
    return net::IoStatus::Ok;
}

Result UsbChannel::poll(UsbDsEndpoint* ep, u32 urb, Urb* state, UsbDsReportEntry* entry) {
    UsbDsReportData rd{};
    const Result rc = usbDsEndpoint_GetReportData(ep, &rd);
    *state = Urb::Pending;
    if (R_FAILED(rc)) return rc;
    const u32 count = std::min<u32>(rd.report_count, 8);
    for (u32 i = 0; i < count; ++i) {
        const UsbDsReportEntry& e = rd.report[i];
        if (e.id != urb) {
            ++strayReports_;
            continue;
        }
        *entry = e;
        if (e.urb_status == kUrbFinished) *state = Urb::Done;
        else if (e.urb_status <= kUrbInProgress) *state = Urb::Pending;
        else *state = Urb::Failed;
    }
    return 0;
}

bool UsbChannel::drainIn(int budgetMs) {
    const uint64_t deadline = sys::nowMs() + static_cast<uint64_t>(budgetMs);
    bool first = true;
    while (inPending_) {
        if (!first) {
            const uint64_t now = sys::nowMs();
            if (now >= deadline) return false;
            Result rc = 0;
            const uint64_t before = sys::nowMs();
            waitEvent(&in_->CompletionEvent, static_cast<int>(std::min<uint64_t>(deadline - now, kDrainSliceMs)), &rc);
            if (R_FAILED(rc) && sys::nowMs() == before) svcSleepThread(static_cast<int64_t>(kDrainSliceMs) * 1000000);
            eventClear(&in_->CompletionEvent);
        }
        first = false;
        Urb state;
        UsbDsReportEntry e{};
        if (R_FAILED(poll(in_, inUrb_, &state, &e))) return false;
        if (state != Urb::Pending) {
            inPending_ = false;
            break;
        }
    }
    return true;
}

bool UsbChannel::drainOut(int budgetMs) {
    const uint64_t deadline = sys::nowMs() + static_cast<uint64_t>(budgetMs);
    bool first = true;
    while (outPending_) {
        if (!first) {
            const uint64_t now = sys::nowMs();
            if (now >= deadline) return false;
            Result rc = 0;
            const uint64_t before = sys::nowMs();
            waitEvent(&out_->CompletionEvent, static_cast<int>(std::min<uint64_t>(deadline - now, kDrainSliceMs)), &rc);
            if (R_FAILED(rc) && sys::nowMs() == before) svcSleepThread(static_cast<int64_t>(kDrainSliceMs) * 1000000);
            eventClear(&out_->CompletionEvent);
        }
        first = false;
        Urb state;
        UsbDsReportEntry e{};
        if (R_FAILED(poll(out_, outUrb_, &state, &e))) return false;
        if (state != Urb::Pending) {
            outPending_ = false;
            break;
        }
    }
    return true;
}

bool UsbChannel::cancelAndDrainIn() {
    usbDsEndpoint_Cancel(in_);
    if (drainIn(kDrainBudgetMs)) return true;
    markStuck();
    return false;
}

bool UsbChannel::finishIn(u32 requested, int stallTimeoutMs, bool final) {
    const uint64_t start = sys::nowMs();
    const int slice = std::min(kSliceMs, stallTimeoutMs);
    while (true) {
        const bool alive = final ? (open_.load() && !broken_.load() && !power::sleeping.load() && !power::halted())
                                 : usable();
        if (!alive) {
            cancelAndDrainIn();
            return false;
        }
        Result rc = 0;
        if (waitEvent(&in_->CompletionEvent, slice, &rc)) {
            eventClear(&in_->CompletionEvent);
            Urb state;
            UsbDsReportEntry e{};
            rc = poll(in_, inUrb_, &state, &e);
            if (R_FAILED(rc)) {
                fail("write report", rc);
                broken_ = true;
                cancelAndDrainIn();
                return false;
            }
            if (state == Urb::Done) {
                inPending_ = false;
                if (e.transferredSize == requested) return true;
                if (configured())
                    LOG("usb: write urb %u moved %u of %u bytes", static_cast<unsigned>(e.id),
                        static_cast<unsigned>(e.transferredSize), static_cast<unsigned>(requested));
                return false;
            }
            if (state == Urb::Failed) {
                inPending_ = false;
                if (configured())
                    LOG("usb: write urb %u ended with status %u, %u of %u bytes", static_cast<unsigned>(e.id),
                        static_cast<unsigned>(e.urb_status), static_cast<unsigned>(e.transferredSize),
                        static_cast<unsigned>(requested));
                return false;
            }
        } else if (rc != kTimedOut) {
            fail("wait write", rc);
            broken_ = true;
            cancelAndDrainIn();
            return false;
        }
        const uint64_t stalled = sys::nowMs() - start;
        if (stalled >= static_cast<uint64_t>(stallTimeoutMs) || !configured()) {
            LOG("usb: write stalled for %llu ms (state %d), cancelling", static_cast<unsigned long long>(stalled),
                lastState_.load());
            cancelAndDrainIn();
            return false;
        }
    }
}

bool UsbChannel::writeAll(const uint8_t* p, size_t size, int stallTimeoutMs, bool final) {
    if (inPending_) return false;
    if (final ? !(open_.load() && !broken_.load() && !power::sleeping.load() && !power::halted()) : !usable())
        return false;
    size_t left = size;
    bool zlp = size % maxPacket_.load() == 0;
    while (left > 0 || zlp) {
        const size_t n = std::min(left, kChunk);
        std::memcpy(g_write, p, n);
        const Result rc = usbDsEndpoint_PostBufferAsync(in_, g_write, n, &inUrb_);
        if (R_FAILED(rc)) {
            if (configured()) fail("post write", rc);
            return false;
        }
        inPending_ = true;
        if (!finishIn(static_cast<u32>(n), stallTimeoutMs, final)) return false;
        sentBytes_ += n;
        p += n;
        left -= n;
        if (n == 0) zlp = false;
    }
    return true;
}

bool UsbChannel::sendAll(const void* buf, size_t size, int stallTimeoutMs) {
    if (size == 0) return true;
    sys::Lock lock(inMutex_);
    return writeAll(static_cast<const uint8_t*>(buf), size, stallTimeoutMs, false);
}

void UsbChannel::sendBye() {
    if (!open_.load() || broken_.load() || power::sleeping.load() || power::halted() || !configured()) return;
    uint8_t msg[proto::kHeaderSize];
    proto::encodeHeader(msg, proto::MsgType::Bye, 0, 0);
    sys::Lock lock(inMutex_);
    const bool ok = writeAll(msg, sizeof msg, kByeTimeoutMs, true);
    LOG("usb: bye %s", ok ? "sent" : "not delivered");
}

void UsbChannel::kick() {
    if (!open_.load()) return;
    kicked_ = true;
}

void UsbChannel::flush() {
    sys::Lock inLock(inMutex_);
    sys::Lock outLock(outMutex_);
    if (in_ && out_) {
        usbDsEndpoint_Cancel(in_);
        usbDsEndpoint_Cancel(out_);
        const bool inDone = drainIn(kDrainBudgetMs);
        const bool outDone = drainOut(kDrainBudgetMs);
        if (!inDone || !outDone) {
            markStuck();
            return;
        }
    }
    outPending_ = false;
    ringHead_ = ringTail_ = 0;
}

void UsbChannel::close() {
    if (!open_.load()) return;
    sendBye();
    LOG("usb: session over, %llu KB sent, %llu KB received, %u stale report entries",
        static_cast<unsigned long long>(sentBytes_ / 1024), static_cast<unsigned long long>(receivedBytes_ / 1024),
        static_cast<unsigned>(strayReports_.exchange(0)));
    flush();
    kicked_ = false;
}
