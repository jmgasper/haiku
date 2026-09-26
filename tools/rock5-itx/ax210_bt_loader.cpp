/* AX210 USB bootloader firmware upload for the ROCK 5 ITX Haiku image.
 * The Intel bootloader sends Secure Send commands over bulk OUT and their
 * completions over bulk IN. Other HCI commands and vendor events use the
 * control and interrupt endpoints respectively.
 */
#include "usb_raw.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr size_t kRsaHeader = 644;
static constexpr size_t kEcdsaHeader = 320;
static constexpr size_t kPayload = kRsaHeader + kEcdsaHeader;

struct Controller {
    int fd;
    bool secureResultSeen;
    uint8_t secureResult;
    bool bootSeen;
};

static volatile sig_atomic_t sEventTimeout;

static void eventTimeout(int)
{
    sEventTimeout = 1;
}

static uint32_t le32(const uint8_t* data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool readEvent(Controller& controller, uint8_t* data, size_t& length,
    bool bootloaderBulk = false, unsigned timeoutSeconds = 10,
    bool quietTimeout = false)
{
    usb_raw_command command = {};
    command.transfer.interface = 0;
    command.transfer.endpoint = bootloaderBulk ? 2 : 0;
    command.transfer.data = data;
    command.transfer.length = length;
    sEventTimeout = 0;
    alarm(timeoutSeconds);
    const int result = ioctl(controller.fd,
        bootloaderBulk ? B_USB_RAW_COMMAND_BULK_TRANSFER
            : B_USB_RAW_COMMAND_INTERRUPT_TRANSFER,
        &command, sizeof(command));
    alarm(0);
    length = command.transfer.length;
    if (quietTimeout && sEventTimeout)
        return false;
    if (result < 0 || command.transfer.status != B_USB_RAW_STATUS_SUCCESS) {
        fprintf(stderr, "interrupt transfer failed: ioctl=%d status=%ld\n",
            result, (long)command.transfer.status);
        return false;
    }
    if (length < 2 || (size_t)data[1] + 2 > length) {
        fprintf(stderr, "malformed HCI event: %zu bytes\n", length);
        return false;
    }
    if (data[0] == 0xff && length >= 3) {
        if (data[2] == 0x06 && length >= 7) {
            controller.secureResultSeen = true;
            controller.secureResult = data[3];
            fprintf(stderr, "Intel secure-send result=%u status=%u\n",
                (unsigned)data[3], (unsigned)data[6]);
        } else if (data[2] == 0x02 && length >= 9) {
            controller.bootSeen = true;
            fprintf(stderr, "Intel bootup event received\n");
        }
    }
    return true;
}

static bool command(Controller& controller, uint16_t opcode,
    const uint8_t* payload, size_t payloadLength, uint8_t* reply = nullptr,
    size_t* replyLength = nullptr)
{
    if (payloadLength > 255)
        return false;
    uint8_t packet[258] = { (uint8_t)opcode, (uint8_t)(opcode >> 8),
        (uint8_t)payloadLength };
    if (payloadLength != 0)
        memcpy(packet + 3, payload, payloadLength);
    usb_raw_command transfer = {};
    int result;
    if (opcode == 0xfc09) {
        transfer.transfer.interface = 0;
        transfer.transfer.endpoint = 1;
        transfer.transfer.data = packet;
        transfer.transfer.length = payloadLength + 3;
        alarm(10);
        result = ioctl(controller.fd, B_USB_RAW_COMMAND_BULK_TRANSFER,
            &transfer, sizeof(transfer));
        alarm(0);
        if (result < 0 || transfer.transfer.status != B_USB_RAW_STATUS_SUCCESS
            || transfer.transfer.length != payloadLength + 3) {
            fprintf(stderr, "bulk command %04x failed: ioctl=%d status=%ld bytes=%zu\n",
                opcode, result, (long)transfer.transfer.status,
                transfer.transfer.length);
            return false;
        }
    } else {
        transfer.control.request_type = 0x20;
        transfer.control.request = 0;
        transfer.control.value = 0;
        transfer.control.index = 0;
        transfer.control.length = payloadLength + 3;
        transfer.control.data = packet;
        alarm(10);
        result = ioctl(controller.fd, B_USB_RAW_COMMAND_CONTROL_TRANSFER,
            &transfer, sizeof(transfer));
        alarm(0);
        if (result < 0 || transfer.control.status != B_USB_RAW_STATUS_SUCCESS
            || transfer.control.length != payloadLength + 3) {
            fprintf(stderr, "command %04x control transfer failed: ioctl=%d status=%ld bytes=%u\n",
                opcode, result, (long)transfer.control.status,
                transfer.control.length);
            return false;
        }
    }
    // Intel's bootloader does not acknowledge the boot-to-operational reset.
    if (opcode == 0xfc01)
        return true;
    for (int i = 0; i < 16; i++) {
        uint8_t event[256] = {};
        size_t length = sizeof(event);
        if (!readEvent(controller, event, length, opcode == 0xfc09))
            return false;
        if (event[0] != 0x0e || length < 6)
            continue;
        const uint16_t responseOpcode = (uint16_t)event[3]
            | ((uint16_t)event[4] << 8);
        if (responseOpcode != opcode) {
            fprintf(stderr, "unexpected completion opcode %04x, wanted %04x\n",
                responseOpcode, opcode);
            continue;
        }
        if (event[5] != 0) {
            fprintf(stderr, "command %04x HCI status=%u\n", opcode,
                (unsigned)event[5]);
            return false;
        }
        if (reply != nullptr && replyLength != nullptr) {
            if (*replyLength < length - 6)
                return false;
            memcpy(reply, event + 6, length - 6);
            *replyLength = length - 6;
        }
        return true;
    }
    fprintf(stderr, "command %04x completion not found\n", opcode);
    return false;
}

static bool readVersion(Controller& controller, uint8_t& image,
    uint8_t& secureBootEngine)
{
    const uint8_t parameter = 0xff;
    uint8_t reply[256] = {};
    size_t length = sizeof(reply);
    if (!command(controller, 0xfc05, &parameter, 1, reply, &length))
        return false;
    image = 0xff;
    secureBootEngine = 0xff;
    for (size_t offset = 0; offset < length;) {
        if (offset + 2 > length || offset + 2 + reply[offset + 1] > length)
            return false;
        const uint8_t type = reply[offset];
        const uint8_t size = reply[offset + 1];
        if (type == 0x1c && size == 1)
            image = reply[offset + 2];
        if (type == 0x2f && size == 1)
            secureBootEngine = reply[offset + 2];
        offset += 2 + size;
    }
    printf("Intel image=%u secure_boot_engine=%u\n",
        (unsigned)image, (unsigned)secureBootEngine);
    fflush(stdout);
    return image != 0xff;
}

static bool secureSend(Controller& controller, uint8_t type,
    const uint8_t* data, size_t length)
{
    while (length > 0) {
        const size_t part = length > 252 ? 252 : length;
        uint8_t payload[253];
        payload[0] = type;
        memcpy(payload + 1, data, part);
        if (!command(controller, 0xfc09, payload, part + 1))
            return false;
        data += part;
        length -= part;
    }
    return true;
}

static bool sendFirmware(Controller& controller, const uint8_t* firmware,
    size_t size, uint32_t& bootAddress)
{
    if (size <= kPayload || le32(firmware + 8) != 0x00010000
        || firmware[kRsaHeader] != 0x06
        || le32(firmware + kRsaHeader + 8) != 0x00020000) {
        fprintf(stderr, "invalid AX210 SFI header\n");
        return false;
    }
    bootAddress = 0;
    size_t groupStart = kPayload;
    size_t groupLength = 0;
    size_t fragments = 0;
    fprintf(stderr, "sending ECDSA CSS header\n"); fflush(stderr);
    if (!secureSend(controller, 0, firmware + 644, 128)) return false;
    fprintf(stderr, "sending ECDSA public key\n"); fflush(stderr);
    if (!secureSend(controller, 3, firmware + 772, 96)) return false;
    fprintf(stderr, "sending ECDSA signature\n"); fflush(stderr);
    if (!secureSend(controller, 2, firmware + 868, 96)) return false;
    fprintf(stderr, "sending SFI command stream\n"); fflush(stderr);
    for (size_t offset = kPayload; offset < size;) {
        if (offset + 3 > size || offset + 3 + firmware[offset + 2] > size) {
            fprintf(stderr, "truncated SFI command at %zu\n", offset);
            return false;
        }
        const uint16_t opcode = firmware[offset]
            | ((uint16_t)firmware[offset + 1] << 8);
        const size_t commandSize = 3 + firmware[offset + 2];
        if (opcode == 0xfc0e && commandSize >= 7)
            bootAddress = le32(firmware + offset + 3);
        offset += commandSize;
        groupLength += commandSize;
        if (groupLength % 4 != 0)
            continue;
        if (groupLength > 252) {
            fprintf(stderr, "oversize SFI fragment: %zu\n", groupLength);
            return false;
        }
        if (!secureSend(controller, 1, firmware + groupStart, groupLength))
            return false;
        fragments++;
        if (fragments <= 3 || (fragments % 256) == 0) {
            fprintf(stderr, "sent %zu firmware fragments\n", fragments);
            fflush(stderr);
        }
        groupStart = offset;
        groupLength = 0;
    }
    if (groupLength != 0 || bootAddress == 0)
        return false;
    fprintf(stderr, "sent %zu firmware fragments, boot address=%08lx\n",
        fragments, (unsigned long)bootAddress);
    return true;
}

static bool applyDdc(Controller& controller, const char* path)
{
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        perror("open Intel DDC");
        return false;
    }
    uint8_t data[4096];
    size_t length = fread(data, 1, sizeof(data), file);
    const bool complete = feof(file) != 0;
    fclose(file);
    if (!complete || length == 0) {
        fprintf(stderr, "invalid Intel DDC length\n");
        return false;
    }
    size_t records = 0;
    for (size_t offset = 0; offset < length;) {
        size_t size = (size_t)data[offset] + 1;
        if (size < 4 || offset + size > length) {
            fprintf(stderr, "invalid Intel DDC record at %zu\n", offset);
            return false;
        }
        if (!command(controller, 0xfc8b, data + offset, size))
            return false;
        offset += size;
        records++;
    }
    printf("applied %zu Intel DDC records\n", records);
    return true;
}

static bool probeHci(Controller& controller)
{
    uint8_t reply[7];
    size_t length = sizeof(reply);
    if (!command(controller, 0x1005, nullptr, 0, reply, &length)
        || length != sizeof(reply)) {
        fprintf(stderr, "Bluetooth Read Buffer Size failed\n");
        return false;
    }
    const unsigned aclSize = reply[0] | ((unsigned)reply[1] << 8);
    const unsigned aclPackets = reply[3] | ((unsigned)reply[4] << 8);
    printf("Bluetooth HCI ready: ACL bytes=%u packets=%u\n", aclSize,
        aclPackets);

    uint8_t features[8];
    length = sizeof(features);
    if (!command(controller, 0x2003, nullptr, 0, features, &length)
        || length != sizeof(features)) {
        fprintf(stderr, "Bluetooth LE Read Local Supported Features failed\n");
        return false;
    }
    printf("Bluetooth LE features=");
    for (size_t i = 0; i < length; i++)
        printf("%02x", features[i]);
    printf("\n");
    return true;
}

static bool scanLe(Controller& controller)
{
    // Keep the Classic events used by bluetooth_server enabled as well.
    // These masks follow the BR/EDR and LE event selections in Linux HCI.
    const uint8_t eventMask[8] = {
        0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d
    };
    const uint8_t leEventMask[8] = { 0x02, 0, 0, 0, 0, 0, 0, 0 };
    // Active scan, 10 ms interval/window, public own address, accept all.
    const uint8_t scanParameters[7] = { 1, 0x10, 0, 0x10, 0, 0, 0 };
    const uint8_t enable[2] = { 1, 1 };
    const uint8_t disable[2] = { 0, 0 };
    if (!command(controller, 0x0c01, eventMask, sizeof(eventMask))
        || !command(controller, 0x2001, leEventMask, sizeof(leEventMask))
        || !command(controller, 0x200b, scanParameters,
            sizeof(scanParameters))
        || !command(controller, 0x200c, enable, sizeof(enable))) {
        fprintf(stderr, "Bluetooth LE scan setup failed\n");
        return false;
    }

    struct sigaction action = {};
    struct sigaction previous = {};
    action.sa_handler = eventTimeout;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, &previous);

    unsigned reports = 0;
    for (int i = 0; i < 6; i++) {
        uint8_t event[260] = {};
        size_t length = sizeof(event);
        if (readEvent(controller, event, length, false, 1, true)
            && event[0] == 0x3e && length >= 4
            && (event[2] == 0x02 || event[2] == 0x0d)) {
            reports += event[3];
        }
    }
    const bool disabled = command(controller, 0x200c, disable,
        sizeof(disable));
    sigaction(SIGALRM, &previous, nullptr);
    printf("Bluetooth LE advertising reports=%u\n", reports);
    return disabled;
}

static bool isAx210(int fd)
{
    usb_raw_command probe = {};
    usb_device_descriptor descriptor = {};
    probe.device.descriptor = &descriptor;
    return ioctl(fd, B_USB_RAW_COMMAND_GET_DEVICE_DESCRIPTOR, &probe,
            sizeof(probe)) >= 0
        && probe.device.status == B_USB_RAW_STATUS_SUCCESS
        && descriptor.vendor_id == 0x8087 && descriptor.product_id == 0x0032;
}

static int findController(const char* directoryPath, char* found,
    size_t foundSize, unsigned depth)
{
    if (depth > 5)
        return -1;
    DIR* directory = opendir(directoryPath);
    if (directory == nullptr)
        return -1;
    int fd = -1;
    struct dirent* entry;
    while ((entry = readdir(directory)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0)
            continue;
        char path[1024];
        const int length = snprintf(path, sizeof(path), "%s/%s",
            directoryPath, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path))
            continue;
        struct stat info;
        if (stat(path, &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode))
            fd = findController(path, found, foundSize, depth + 1);
        else {
            fd = open(path, O_RDWR);
            if (fd >= 0 && !isAx210(fd)) {
                close(fd);
                fd = -1;
            }
            if (fd >= 0)
                snprintf(found, foundSize, "%s", path);
        }
        if (fd >= 0)
            break;
    }
    closedir(directory);
    return fd;
}

int main(int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "--present") == 0) {
        char found[1024] = {};
        const int probe = findController("/dev/bus/usb", found,
            sizeof(found), 0);
        if (probe < 0)
            return 1;
        close(probe);
        return 0;
    }
    const bool leScan = argc == 5 && strcmp(argv[4], "--le-scan") == 0;
    if ((argc != 3 && argc != 4) && !leScan) {
        fprintf(stderr, "usage: %s --present | USB_RAW_DEVICE|auto AX210_SFI [INTEL_DDC [--le-scan]]\n", argv[0]);
        return 2;
    }
    char found[1024] = {};
    const int fd = strcmp(argv[1], "auto") == 0
        ? findController("/dev/bus/usb", found, sizeof(found), 0)
        : open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("find AX210 USB device");
        return 1;
    }
    if (!isAx210(fd)) {
        fprintf(stderr, "USB device is not Intel AX210 Bluetooth\n");
        return 1;
    }
    if (found[0] != '\0')
        printf("AX210 USB device: %s\n", found);
    Controller controller = { fd, false, 0xff, false };
    uint8_t image, sbe;
    if (!readVersion(controller, image, sbe))
        return 1;
    if (image == 3) {
        printf("AX210 Bluetooth firmware already operational\n");
        if (argc >= 4 && !applyDdc(controller, argv[3]))
            return 1;
        if (!probeHci(controller))
            return 1;
        return !leScan || scanLe(controller) ? 0 : 1;
    }
    if (image != 1 || sbe != 1) {
        fprintf(stderr, "unsupported AX210 bootloader image or security mode\n");
        return 1;
    }
    FILE* file = fopen(argv[2], "rb");
    if (file == nullptr) { perror("open firmware"); return 1; }
    fseek(file, 0, SEEK_END);
    const long fileSize = ftell(file);
    rewind(file);
    if (fileSize < (long)kPayload || fileSize > 2 * 1024 * 1024) {
        fprintf(stderr, "unexpected SFI size %ld\n", fileSize);
        return 1;
    }
    uint8_t* firmware = (uint8_t*)malloc((size_t)fileSize);
    if (firmware == nullptr || fread(firmware, 1, (size_t)fileSize, file) != (size_t)fileSize)
        return 1;
    fclose(file);
    fprintf(stderr, "read %ld SFI bytes\n", fileSize); fflush(stderr);
    uint32_t bootAddress;
    if (!sendFirmware(controller, firmware, (size_t)fileSize, bootAddress))
        return 1;
    free(firmware);
    for (int i = 0; !controller.secureResultSeen && i < 8; i++) {
        uint8_t event[256]; size_t length = sizeof(event);
        if (!readEvent(controller, event, length))
            return 1;
    }
    if (!controller.secureResultSeen || controller.secureResult != 0) {
        fprintf(stderr, "Intel secure-send did not complete successfully\n");
        return 1;
    }
    uint8_t reset[8] = { 0, 1, 0, 1,
        (uint8_t)bootAddress, (uint8_t)(bootAddress >> 8),
        (uint8_t)(bootAddress >> 16), (uint8_t)(bootAddress >> 24) };
    if (!command(controller, 0xfc01, reset, sizeof(reset)))
        return 1;
    for (int i = 0; !controller.bootSeen && i < 8; i++) {
        uint8_t event[256]; size_t length = sizeof(event);
        if (!readEvent(controller, event, length))
            return 1;
    }
    if (!controller.bootSeen) {
        fprintf(stderr, "Intel operational boot event missing\n");
        return 1;
    }
    if (!readVersion(controller, image, sbe) || image != 3)
        return 1;
    printf("AX210 Bluetooth firmware operational\n");
    if (argc >= 4 && !applyDdc(controller, argv[3]))
        return 1;
    if (!probeHci(controller))
        return 1;
    return !leScan || scanLe(controller) ? 0 : 1;
}
