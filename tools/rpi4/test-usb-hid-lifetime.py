#!/usr/bin/env python3
"""Exercise the actual USB HID lifecycle hooks with ASan/UBSan on the host.

The USB and device-list adapters are fixtures. Open/close/removal/free hooks
are extracted unchanged from the production driver, so close-before-unplug
and multiple outstanding file cookies cover the original use-after-free.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = root / 'src/add-ons/kernel/drivers/input/usb_hid'


def function(text, name):
    start = text.index('\n' + name + '(') + 1
    start = text.rfind('\n', 0, start - 1) + 1
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


device = (source / 'HIDDevice.cpp').read_text()
driver = (source / 'Driver.cpp').read_text()
hooks = ''.join(function(device, name) for name in (
    'HIDDevice::Open', 'HIDDevice::Close', 'HIDDevice::Removed'))
# The pre-fix source has no separate free-time reference release.
if '\nHIDDevice::ReleaseOpenReference(' in device:
    hooks += function(device, 'HIDDevice::ReleaseOpenReference')
hooks += function(driver, 'usb_hid_device_removed')
hooks += function(driver, 'usb_hid_free')

fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
using int32 = int32_t;
using uint32 = uint32_t;
using addr_t = uintptr_t;
using status_t = int;
constexpr int B_OK = 0;
#define TRACE(...) ((void)0)
static int sDriverLock;
void mutex_lock(int*) {}
void mutex_unlock(int*) {}
int atomic_add(int* value, int n) { int old = *value; *value += n; return old; }
struct USB { void cancel_queued_transfers(int) {} } usb;
USB* gUSBModule = &usb;
class ProtocolHandler;
static int destroyed;
class HIDDevice {
public:
    int fOpenCount = 0;
    int fOpenReferences = 0;
    int fInterruptPipe = 1;
    bool fRemoved = false;
    std::vector<ProtocolHandler*> handlers;
    ~HIDDevice();
    bool IsOpen() const { return fOpenCount > 0; }
    bool HasOpenReferences() const { return fOpenReferences > 0; }
    bool IsRemoved() const { return fRemoved; }
    int ParentCookie() const { return 1; }
    ProtocolHandler* ProtocolHandlerAt(unsigned i) {
        return i < handlers.size() ? handlers[i] : nullptr;
    }
    status_t Open(ProtocolHandler*, uint32);
    status_t Close(ProtocolHandler*);
    void ReleaseOpenReference();
    void Removed();
};
class ProtocolHandler {
public:
    HIDDevice* device;
    explicit ProtocolHandler(HIDDevice* d) : device(d) {}
    HIDDevice* Device() { return device; }
};
HIDDevice::~HIDDevice() { ++destroyed; for (auto* h : handlers) delete h; }
struct DeviceList {
    std::vector<ProtocolHandler*> entries;
    int CountDevices() { return entries.size(); }
    void* DeviceAt(int i) { return i >= 0 && i < CountDevices() ? entries[i] : nullptr; }
    void RemoveDevice(const char*, ProtocolHandler* p) {
        entries.erase(std::find(entries.begin(), entries.end(), p));
    }
} list;
DeviceList* gDeviceList = &list;
struct device_cookie { ProtocolHandler* handler; uint32 cookie = 0; };
'''
tests = r'''
int main() {
    for (int count : {1, 2, 3}) {
        for (int closedFirst = 0; closedFirst <= count; ++closedFirst) {
            destroyed = 0;
            auto* device = new HIDDevice;
            std::vector<device_cookie*> cookies;
            for (int i = 0; i < count; ++i) {
                auto* handler = new ProtocolHandler(device);
                device->handlers.push_back(handler);
                list.entries.push_back(handler);
                assert(device->Open(handler, 0) == B_OK);
                cookies.push_back(new device_cookie{handler});
            }
            // VFS close and free are separate hooks. Disconnect may happen
            // between them, with one or several file cookies outstanding.
            for (int i = 0; i < closedFirst; ++i)
                device->Close(cookies[i]->handler);
            usb_hid_device_removed((void*)(addr_t)1);
            assert(list.entries.empty());
            assert(destroyed == 0);
            for (int i = 0; i < count; ++i) {
                if (i >= closedFirst) device->Close(cookies[i]->handler);
                usb_hid_free(cookies[i]);
                assert(destroyed == (i == count - 1 ? 1 : 0));
            }
        }
    }
    // All files freed while still attached: deletion belongs to removal.
    destroyed = 0;
    auto* device = new HIDDevice;
    auto* handler = new ProtocolHandler(device);
    device->handlers.push_back(handler);
    list.entries.push_back(handler);
    device->Open(handler, 0);
    device->Close(handler);
    usb_hid_free(new device_cookie{handler});
    assert(destroyed == 0);
    usb_hid_device_removed((void*)(addr_t)1);
    assert(destroyed == 1);
    puts("USB HID close/remove/free interleavings: passed");
}
'''
with tempfile.TemporaryDirectory(prefix='usb-hid-life-', dir='/mnt/HaikuWork/tmp') as work:
    path = Path(work)
    (path / 'test.cpp').write_text(fixture + hooks + tests)
    subprocess.run(['c++', '-std=c++17', '-g', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(path / 'test.cpp'), '-o',
                    str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
