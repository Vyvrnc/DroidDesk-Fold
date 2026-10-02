# USB disky — test on the phone (fold.19)

Use sticks whose data is not needed (formatting erases them). Native XFCE terminal unless noted.

1. Tools: `pkg install dosfstools exfatprogs e2fsprogs mtools` and for NTFS
   `pkg install root-repo && pkg install ntfs-3g` (pkg does not run as root from Debian).
2. Speed, 100 MB of random data up and down: Alcor FAT32 and ext4, SD card FAT32.
   fold.18 (before the cache): Alcor FAT32 2 / 3.7 MB/s, ext4 0.6 / 10 MB/s, SD 5 / 18 MB/s.
3. exFAT: `mkdir`, `cp` of a folder (with a Czech file name), `ls`, `cp` back + `diff -r`, `rm -r`;
   the same in the Soubory… window.
4. NTFS: `droiddesk-usb format ntfs --label FOLDNTFS` (shows the disk contents first), `info`,
   the same file operations, 100 MB up/down.
5. Another computer: read the NTFS and exFAT sticks; on Windows run `chkdsk X:` on NTFS once.
6. Concurrency: delete from the window and `droiddesk-usb ls` at the same time → the second waits
   ("Čekám, až doběhne jiná operace s tímto diskem…") and finishes; a killed `cp` leaves no mcopy.
7. `DROIDDESK_BLK_DEBUG=1 droiddesk-usb ls usb:/` finishes (it hung before fold.19).
8. From Debian: `droiddesk-usb cp /root/x usb:/` and back.

Debugging: `DROIDDESK_BLK_DEBUG=1` (every call of the shim on stderr of the tool),
`logcat -s UsbBridge UsbFlasher`.
