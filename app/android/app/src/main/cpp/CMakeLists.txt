cmake_minimum_required(VERSION 3.22.1)

project("droiddesk_compositor")

# Add the native JNI bridge source
add_library(
    droiddesk_compositor
    SHARED
    compositor_jni.cpp
)

# Search for standard Android libraries
find_library(log-lib log)
find_library(android-lib android)

# Link the libraries
target_link_libraries(
    droiddesk_compositor
    ${log-lib}
    ${android-lib}
)

# droiddesk-usb: USBDEVFS ioctls Android's UsbDeviceConnection does not offer.
add_library(droiddesk_usb SHARED usb_jni.c)

# droiddesk-usb format/files: LD_PRELOAD shim that makes /dev/droiddesk-blk the held disk.
# Not loaded by the app; copied to $PREFIX/lib for the Termux tools.
add_library(droiddesk_blk SHARED blk_shim.c)
target_compile_options(droiddesk_blk PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wall)
target_link_libraries(droiddesk_blk dl)
