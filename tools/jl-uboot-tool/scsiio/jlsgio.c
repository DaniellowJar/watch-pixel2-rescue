/*
 * jlsgio.c - macOS SCSI passthrough helper for the JieLi UBOOT tool.
 *
 * Sends raw SCSI CDBs to any USB Mass Storage (BOT) device via the
 * IOKit SCSITaskUserClient interface (same API as Apple's
 * "STUCOtherDeviceTool" sample and superg/redumper).
 *
 * Build:
 *   clang -O2 -o jlsgio jlsgio.c -framework IOKit -framework CoreFoundation
 *
 * Usage:
 *   jlsgio scan
 *       List USB SCSI-task devices: one line per device:
 *       PATH \t VENDOR \t PRODUCT \t REV
 *       (PATH is a /dev/diskN name)
 *
 *   jlsgio exec --dev /dev/disk4 --cdb <hex> [--dout <hex>] [--din <len>] [--timeout <ms>]
 *       Send a CDB. On data-in, prints DATA=<hex>. Prints:
 *       STATUS=<SCSITaskStatus> SERVICE=<SCSIServiceResponse> XFER=<bytes>
 *       SENSE=<hex> [ERROR=<msg>]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/scsi/SCSICommandOperationCodes.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/storage/IOMedia.h>

static void die(const char *msg)
{
    fprintf(stderr, "jlsgio: %s\n", msg);
    exit(1);
}

/* Not pulled in by user-space headers; values from IOKit/scsi/SCSITask.h */
#ifndef kSCSIDataTransfer_FromInitiatorToTarget
#define kSCSIDataTransfer_NoDataTransfer         0x00
#define kSCSIDataTransfer_FromInitiatorToTarget  0x01
#define kSCSIDataTransfer_FromTargetToInitiator  0x02
#endif

/* Find the IOService of the SCSITaskUserClient nub for a BSD disk name. */
static io_service_t find_task_service_by_bsd(const char *bsd)
{
    CFMutableDictionaryRef matcher = IOBSDNameMatching(kIOMainPortDefault, 0, bsd);
    if (!matcher)
        return 0;

    io_service_t media = IOServiceGetMatchingService(kIOMainPortDefault, matcher);
    if (!media)
        return 0;

    io_registry_entry_t cur = media;
    io_service_t found = 0;

    for (;;) {
        CFTypeRef cat = IORegistryEntryCreateCFProperty(
            cur, CFSTR(kIOPropertySCSITaskDeviceCategory), kCFAllocatorDefault, 0);

        if (cat) {
            int match = 0;
            if (CFGetTypeID(cat) == CFStringGetTypeID()) {
                if (CFStringCompare((CFStringRef)cat,
                                    CFSTR(kIOPropertySCSITaskUserClientDevice), 0) == kCFCompareEqualTo)
                    match = 1;
            }
            CFRelease(cat);
            if (match) {
                IOObjectRetain(cur);
                found = cur;
                break;
            }
        }

        io_registry_entry_t parent = 0;
        kern_return_t kr = IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent);
        IOObjectRelease(cur);
        if (kr != kIOReturnSuccess || !parent)
            break;
        if (parent == cur)
            break;
        cur = parent;
    }

    return found;
}

/* Does this service (or an ancestor) belong to a USB device? */
static int is_usb_service(io_service_t service)
{
    io_registry_entry_t cur = service;
    int usb = 0;

    while (cur) {
        if (IOObjectConformsTo(cur, kIOUSBDeviceClassName)) {
            usb = 1;
            io_registry_entry_t tmp = cur;
            IOObjectRelease(tmp);
            break;
        }
        io_registry_entry_t parent = 0;
        kern_return_t kr = IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent);
        if (kr != kIOReturnSuccess || !parent)
            break;
        io_registry_entry_t tmp = cur;
        IOObjectRelease(tmp);
        cur = parent;
    }
    return usb;
}

/* Find a BSD /dev/diskN name for a SCSITask service (search its IOMedia child). */
static int get_bsd_name(io_service_t service, char *out, size_t outsz)
{
    io_iterator_t iter = 0;
    kern_return_t kr = IORegistryEntryCreateIterator(
        service, kIOServicePlane, kIORegistryIterateRecursively, &iter);
    if (kr != kIOReturnSuccess)
        return -1;

    io_registry_entry_t child;
    while ((child = IOIteratorNext(iter))) {
        if (IOObjectConformsTo(child, "IOMedia")) {
            CFTypeRef bsd = IORegistryEntryCreateCFProperty(
                child, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0);
            if (bsd) {
                if (CFGetTypeID(bsd) == CFStringGetTypeID()) {
                    char tmp[256] = {0};
                    if (CFStringGetCString((CFStringRef)bsd, tmp, sizeof(tmp), kCFStringEncodingUTF8)) {
                        snprintf(out, outsz, "%s", tmp);
                        CFRelease(bsd);
                        IOObjectRelease(child);
                        IOObjectRelease(iter);
                        return 0;
                    }
                }
                CFRelease(bsd);
            }
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(iter);
    return -1;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t maxlen, size_t *outlen)
{
    size_t n = strlen(hex);
    if (n & 1)
        return -1;
    if (n / 2 > maxlen)
        return -2;
    for (size_t i = 0; i < n; i += 2) {
        int hi = hexval(hex[i]);
        int lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return -3;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *outlen = n / 2;
    return 0;
}

static void print_hex(const uint8_t *buf, size_t len)
{
    printf("DATA=");
    for (size_t i = 0; i < len; i++)
        printf("%02x", buf[i]);
    printf("\n");
}

/*
 * dir: 0 = no data, 1 = data out (initiator->target), 2 = data in (target->initiator)
 */
static int run_scsi(io_service_t service, uint8_t *cdb, size_t cdblen,
                    int dir, uint8_t *buf, size_t buflen, uint32_t timeout_ms,
                    uint8_t *sense_out, size_t *sense_len)
{
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOReturn err = IOCreatePlugInInterfaceForService(
        service, kIOSCSITaskDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    if (err != kIOReturnSuccess) {
        fprintf(stderr, "jlsgio: IOCreatePlugInInterfaceForService failed: 0x%x\n", err);
        return -1;
    }

    SCSITaskDeviceInterface **iface = NULL;
    HRESULT herr = (*plugin)->QueryInterface(
        plugin, CFUUIDGetUUIDBytes(kIOSCSITaskDeviceInterfaceID), (void **)&iface);
    if (herr != S_OK || !iface) {
        fprintf(stderr, "jlsgio: QueryInterface SCSITaskDeviceInterface failed\n");
        IODestroyPlugInInterface(plugin);
        return -1;
    }

    err = (*iface)->ObtainExclusiveAccess(iface);
    if (err != kIOReturnSuccess) {
        fprintf(stderr, "jlsgio: ObtainExclusiveAccess failed: 0x%x (disk mounted? run: diskutil unmountDisk <dev>)\n", err);
        (*iface)->Release(iface);
        IODestroyPlugInInterface(plugin);
        return -1;
    }

    SCSITaskInterface **task = (*iface)->CreateSCSITask(iface);
    if (!task) {
        fprintf(stderr, "jlsgio: CreateSCSITask failed\n");
        (*iface)->ReleaseExclusiveAccess(iface);
        (*iface)->Release(iface);
        IODestroyPlugInInterface(plugin);
        return -1;
    }

    int rc = -1;

    err = (*task)->SetCommandDescriptorBlock(task, cdb, (UInt8)cdblen);
    if (err != kIOReturnSuccess) {
        fprintf(stderr, "jlsgio: SetCommandDescriptorBlock failed: 0x%x\n", err);
        goto out;
    }

    if (buflen > 0) {
        SCSITaskSGElement sg;
        sg.address = (mach_vm_address_t)buf;
        sg.length = buflen;
        UInt32 d = (dir == 1)
            ? kSCSIDataTransfer_FromInitiatorToTarget
            : kSCSIDataTransfer_FromTargetToInitiator;
        err = (*task)->SetScatterGatherEntries(task, &sg, 1, buflen, d);
        if (err != kIOReturnSuccess) {
            fprintf(stderr, "jlsgio: SetScatterGatherEntries failed: 0x%x\n", err);
            goto out;
        }
    }

    err = (*task)->SetTimeoutDuration(task, timeout_ms);
    if (err != kIOReturnSuccess) {
        fprintf(stderr, "jlsgio: SetTimeoutDuration failed: 0x%x\n", err);
        goto out;
    }

    SCSI_Sense_Data sense;
    memset(&sense, 0, sizeof(sense));
    SCSITaskStatus taskStatus = kSCSITaskStatus_GOOD;
    uint64_t transferCount = 0;

    err = (*task)->ExecuteTaskSync(task, &sense, &taskStatus, &transferCount);
    if (err != kIOReturnSuccess) {
        fprintf(stderr, "jlsgio: ExecuteTaskSync failed: 0x%x\n", err);
        goto out;
    }

    SCSIServiceResponse svc = kSCSIServiceResponse_Request_In_Process;
    (*task)->GetSCSIServiceResponse(task, &svc);

    printf("STATUS=%d\n", (int)taskStatus);
    printf("SERVICE=%d\n", (int)svc);
    printf("XFER=%llu\n", (unsigned long long)transferCount);

    if (dir == 2 && buflen > 0)
        print_hex(buf, buflen);

    if (sense_out && sense_len) {
        *sense_len = sizeof(SCSI_Sense_Data);
        memcpy(sense_out, &sense, sizeof(SCSI_Sense_Data));
    }

    rc = (taskStatus == kSCSITaskStatus_GOOD) ? 0 : 1;

out:
    (*task)->Release(task);
    (*iface)->ReleaseExclusiveAccess(iface);
    (*iface)->Release(iface);
    IODestroyPlugInInterface(plugin);
    return rc;
}

static void print_sense_hex(const uint8_t *sense, size_t n)
{
    printf("SENSE=");
    for (size_t i = 0; i < n; i++)
        printf("%02x", sense[i]);
    printf("\n");
}

static int cmd_scan(void)
{
    CFMutableDictionaryRef subDict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(subDict, CFSTR(kIOPropertySCSITaskDeviceCategory),
                         CFSTR(kIOPropertySCSITaskUserClientDevice));

    CFMutableDictionaryRef matchingDict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(matchingDict, CFSTR(kIOPropertyMatchKey), subDict);

    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, matchingDict, &iter);
    CFRelease(subDict);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "jlsgio: IOServiceGetMatchingServices failed\n");
        return 1;
    }

    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        if (is_usb_service(service)) {
            char bsd[64] = {0};
            if (get_bsd_name(service, bsd, sizeof(bsd)) == 0) {
                /* INQUIRY */
                uint8_t cdb[6] = { kSCSICmd_INQUIRY, 0, 0, 0, 36, 0 };
                uint8_t inq[36] = {0};
                size_t sense_len = 0;
                uint8_t sense[32] = {0};
                int rc = run_scsi(service, cdb, 6, 2, inq, 36, 5000, sense, &sense_len);
                if (rc == 0) {
                    char vendor[9] = {0}, product[17] = {0}, rev[5] = {0};
                    memcpy(vendor, inq + 8, 8);
                    memcpy(product, inq + 16, 16);
                    memcpy(rev, inq + 32, 4);
                    for (char *p = vendor; *p; p++) if (*p == ' ') *p = 0;
                    for (char *p = product; *p; p++) if (*p == ' ') *p = 0;
                    printf("/dev/%s\t%s\t%s\t%s\n", bsd, vendor, product, rev);
                }
            }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s scan | exec --dev <path> --cdb <hex> [--dout <hex>] [--din <len>] [--timeout <ms>]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "scan") == 0)
        return cmd_scan();

    if (strcmp(argv[1], "exec") != 0) {
        fprintf(stderr, "jlsgio: unknown mode '%s'\n", argv[1]);
        return 1;
    }

    const char *dev = NULL;
    const char *cdbhex = NULL;
    const char *douthex = NULL;
    long dinlen = -1;
    long timeout = 10000;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dev") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "--cdb") && i + 1 < argc) cdbhex = argv[++i];
        else if (!strcmp(argv[i], "--dout") && i + 1 < argc) douthex = argv[++i];
        else if (!strcmp(argv[i], "--din") && i + 1 < argc) dinlen = atol(argv[++i]);
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = atol(argv[++i]);
        else { fprintf(stderr, "jlsgio: unknown arg '%s'\n", argv[i]); return 1; }
    }

    if (!dev || !cdbhex) {
        fprintf(stderr, "jlsgio: --dev and --cdb are required\n");
        return 1;
    }

    /* /dev/diskN or diskN */
    const char *bsd = dev;
    if (!strncmp(dev, "/dev/disk", 9))
        bsd = dev + 5;
    else if (!strncmp(dev, "disk", 4))
        bsd = dev;

    io_service_t service = find_task_service_by_bsd(bsd);
    if (!service) {
        fprintf(stderr, "jlsgio: no SCSITaskUserClient device found for %s (is it connected?)\n", dev);
        return 1;
    }

    uint8_t cdb[32];
    size_t cdblen = 0;
    if (hex_to_bytes(cdbhex, cdb, sizeof(cdb), &cdblen) != 0) {
        fprintf(stderr, "jlsgio: bad --cdb hex\n");
        IOObjectRelease(service);
        return 1;
    }

    int dir = 0;
    uint8_t bufin[65536];
    uint8_t bufout[65536];
    size_t buflen = 0;
    uint8_t *buf = NULL;

    if (douthex) {
        size_t n = 0;
        if (hex_to_bytes(douthex, bufout, sizeof(bufout), &n) != 0) {
            fprintf(stderr, "jlsgio: bad --dout hex\n");
            IOObjectRelease(service);
            return 1;
        }
        dir = 1;
        buf = bufout;
        buflen = n;
    } else if (dinlen >= 0) {
        if ((size_t)dinlen > sizeof(bufin)) {
            fprintf(stderr, "jlsgio: --din too large\n");
            IOObjectRelease(service);
            return 1;
        }
        memset(bufin, 0, (size_t)dinlen);
        dir = 2;
        buf = bufin;
        buflen = (size_t)dinlen;
    }

    uint8_t sense[32];
    size_t sense_len = 0;
    int rc = run_scsi(service, cdb, cdblen, dir, buf, buflen, (uint32_t)timeout, sense, &sense_len);

    if (sense_len)
        print_sense_hex(sense, sense_len);

    IOObjectRelease(service);
    return rc;
}