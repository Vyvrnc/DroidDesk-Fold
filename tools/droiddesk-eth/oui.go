package main

import (
	"net"
	"strings"
)

// A small built-in OUI table: the devices we actually meet on site. No downloads, the tool
// often runs in networks without internet.
var ouiVendors = map[string]string{
	"70:82:0E": "⭐ Flowbox FBX3100",

	"00:1B:1B": "Siemens", "00:0E:8C": "Siemens", "28:63:36": "Siemens", "8C:F3:19": "Siemens",
	"00:80:F4": "Schneider Electric", "00:00:54": "Schneider Electric",
	"00:90:E8": "Moxa",
	"00:1E:42": "Teltonika",
	"00:A0:45": "Phoenix Contact",
	"00:30:DE": "WAGO",
	"00:01:05": "Beckhoff",
	"4C:5E:0C": "MikroTik", "6C:3B:6B": "MikroTik", "B8:69:F4": "MikroTik", "CC:2D:E0": "MikroTik", "E4:8D:8C": "MikroTik",
	"B8:27:EB": "Raspberry Pi", "DC:A6:32": "Raspberry Pi", "E4:5F:01": "Raspberry Pi", "D8:3A:DD": "Raspberry Pi",
	"28:CD:C1": "Raspberry Pi", "2C:CF:67": "Raspberry Pi",
	"24:0A:C4": "Espressif", "30:AE:A4": "Espressif", "24:6F:28": "Espressif", "A4:CF:12": "Espressif",
	"84:CC:A8": "Espressif", "3C:71:BF": "Espressif", "BC:DD:C2": "Espressif", "EC:FA:BC": "Espressif", "8C:AA:B5": "Espressif",
	"00:E0:4C": "Realtek",
}

func vendorOf(mac net.HardwareAddr) string {
	if len(mac) < 3 {
		return ""
	}
	if v, ok := ouiVendors[strings.ToUpper(mac[:3].String())]; ok {
		return v
	}
	if mac[0]&0x02 != 0 {
		return "lokálně spravovaná MAC"
	}
	return ""
}

// isFlowbox marks the device we look for most often, so a scan can say it found one.
func isFlowbox(mac net.HardwareAddr) bool {
	return len(mac) >= 3 && mac[0] == 0x70 && mac[1] == 0x82 && mac[2] == 0x0e
}
