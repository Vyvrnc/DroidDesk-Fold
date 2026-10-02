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
