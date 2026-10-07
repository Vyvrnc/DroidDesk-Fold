// USB helpers Android's UsbDeviceConnection lacks, for droiddesk-usb.
#include <errno.h>
#include <jni.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>

// USBDEVFS_CLEAR_HALT clears the endpoint halt on the device *and* resets the
// host's data toggle (usb_clear_halt). A plain CLEAR_FEATURE control request only
// resets the device side; the toggles then disagree and every later bulk
// transfer on that endpoint fails. Returns 0 or -errno.
JNIEXPORT jint JNICALL
Java_com_orailnoor_droiddesk_runtime_BotScsiDevice_nativeClearHalt(JNIEnv *env, jclass cls, jint fd, jint endpoint) {
    (void) env;
    (void) cls;
    unsigned int ep = (unsigned int) endpoint;
    return ioctl(fd, USBDEVFS_CLEAR_HALT, &ep) < 0 ? -errno : 0;
}

// USBDEVFS_RESET: port reset of the device, keeping its address (usb_reset_device),
// what Linux usb-storage does when a Bulk-Only reset does not bring a device back.
// Interfaces have to be claimed again afterwards. Returns 0 or -errno.
JNIEXPORT jint JNICALL
Java_com_orailnoor_droiddesk_runtime_BotScsiDevice_nativeReset(JNIEnv *env, jclass cls, jint fd) {
    (void) env;
    (void) cls;
    return ioctl(fd, USBDEVFS_RESET, 0) < 0 ? -errno : 0;
}

// USBDEVFS_BULK with the error kept: Android's bulkTransfer returns -1 for a stall,
// a timeout and a disconnect alike, but Bulk-Only recovery differs (a stall: clear the
// halt and read the CSW; anything else: reset recovery). Returns bytes or -errno.
JNIEXPORT jint JNICALL
Java_com_orailnoor_droiddesk_runtime_BotScsiDevice_nativeBulk(JNIEnv *env, jclass cls, jint fd, jint endpoint,
                                                             jbyteArray buffer, jint offset, jint length, jint timeout) {
    (void) cls;
    if (offset < 0 || length < 0 || offset + length > (*env)->GetArrayLength(env, buffer)) return -EINVAL;
    jbyte *data = (*env)->GetByteArrayElements(env, buffer, NULL);
    if (!data) return -ENOMEM;
    struct usbdevfs_bulktransfer bulk = {
        .ep = (unsigned int) endpoint,
        .len = (unsigned int) length,
        .timeout = (unsigned int) timeout,
        .data = data + offset,
    };
    int n = ioctl(fd, USBDEVFS_BULK, &bulk);
    int result = n < 0 ? -errno : n;
    // Copy back only what an IN transfer filled.
    (*env)->ReleaseByteArrayElements(env, buffer, data, (endpoint & 0x80) ? 0 : JNI_ABORT);
    return result;
}

// Ethernet takeover (UsbEth). Android's releaseInterface hands the interface straight back to
// the kernel driver, and usbfs refuses SETCONFIGURATION while any interface is bound (EBUSY),
// so the configuration switch needs the raw ioctls.

// Detaches (connect = 0) or re-probes (connect = 1) the kernel driver of one interface.
// Returns 0 or -errno (-ENODATA: no driver was bound).
JNIEXPORT jint JNICALL
Java_com_orailnoor_droiddesk_runtime_UsbEth_nativeDriver(JNIEnv *env, jclass cls, jint fd, jint interface,
                                                        jboolean connect) {
    (void) env;
    (void) cls;
    struct usbdevfs_ioctl command = {
        .ifno = interface,
        .ioctl_code = connect ? USBDEVFS_CONNECT : USBDEVFS_DISCONNECT,
        .data = 0,
    };
    return ioctl(fd, USBDEVFS_IOCTL, &command) < 0 ? -errno : 0;
}

// Releases a claimed interface without letting the kernel driver bind again.
JNIEXPORT jint JNICALL
Java_com_orailnoor_droiddesk_runtime_UsbEth_nativeRelease(JNIEnv *env, jclass cls, jint fd, jint interface) {
    (void) env;
    (void) cls;
    unsigned int number = (unsigned int) interface;
    return ioctl(fd, USBDEVFS_RELEASEINTERFACE, &number) < 0 ? -errno : 0;
}

// USBDEVFS_SETCONFIGURATION by bConfigurationValue. Returns 0 or -errno.
JNIEXPORT jint JNICALL
Java_com_orailnoor_droiddesk_runtime_UsbEth_nativeSetConfiguration(JNIEnv *env, jclass cls, jint fd, jint value) {
    (void) env;
    (void) cls;
    int configuration = value;
    return ioctl(fd, USBDEVFS_SETCONFIGURATION, &configuration) < 0 ? -errno : 0;
}
