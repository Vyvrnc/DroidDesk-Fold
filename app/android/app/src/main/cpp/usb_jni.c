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
