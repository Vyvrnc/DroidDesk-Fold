# droiddesk-usb files on NTFS: libntfs-3g (tuxera/ntfs-3g 2026.9.28, GPL-2.0-or-later, unmodified,
# see UPSTREAM) and a small batch tool on top, same protocol as droiddesk_exfat. Shared library
# because the APK only carries those; started through ctypes. Included from ../CMakeLists.txt
# with add_subdirectory(ntfs).
file(GLOB LIBNTFS3G_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/libntfs-3g/*.c)
add_library(droiddesk_ntfs SHARED ntfs_tool.c ${LIBNTFS3G_SOURCES})
target_include_directories(droiddesk_ntfs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ntfs-3g)
target_compile_definitions(droiddesk_ntfs PRIVATE HAVE_CONFIG_H)
target_compile_options(droiddesk_ntfs PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -w)
find_library(ntfs-log-lib log)
if(ntfs-log-lib)
    target_link_libraries(droiddesk_ntfs ${ntfs-log-lib})
endif()
