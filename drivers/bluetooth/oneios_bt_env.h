/*
 * SPDX-License-Identifier: MIT
 *
 * OneiOS Bluetooth: the HCI transport bluetoothd is told to use (no kernel calls: host-tested by
 * drivers/bluetooth/test_bt_env.c).
 *
 * Why (24A446 /usr/sbin/bluetoothd, read in its code; docs/hardware.md 4, "Bluetooth powers on"):
 * - In a virtual machine (kern.hv_vmm_present 1, our /defaults/vmm-present) bluetoothd's power init
 *   (0x100628730) passes only when its transport number is 10 (the chip initialisation at 0x1006294b4
 *   returns `transport == 10`); with H4 (1) it sets PowerState 3 and never opens a transport.
 * - The transport number is the device tree's /bluetooth transport-encoding (or the boot argument
 *   bt-transport-override). The same number also picks the HCI transport (0x10036f8ac), where 10 is
 *   BTVC, a simulated controller inside bluetoothd's own process.
 * - But when the environment has BT_HCI_TRANSPORT, that function takes the HCI transport and driver by
 *   name from it ("TRANSPORT:DRIVER[:DEVICE]", split at ':', matched against the two tables below) and
 *   never reads the number. Nothing else compares the number with 10.
 * So: transport-encoding 10 for the power init, and BT_HCI_TRANSPORT=H4:POSIX for the HCI: H4 framing
 * over the POSIX driver, which opens /dev/btwake and the kernel control com.apple.uart.sk.bluetooth
 * (oneios_bt_sk.c), the same pair the device tree's H4 (1) gives.
 */
#ifndef ONEIOS_BT_ENV_H
#define ONEIOS_BT_ENV_H
#include <stddef.h>
#include <stdbool.h>
#ifdef KERNEL
#include <sys/systm.h>
#else
#include <string.h>
#endif

#define OBT_HCI_ENV_KEY         "BT_HCI_TRANSPORT="
#define OBT_HCI_ENV_DEFAULT     "H4:POSIX"
#define OBT_HCI_ENV_MAX         64u             /* "BT_HCI_TRANSPORT=" + value + NUL */
#define OBT_TRANSPORT_POWER_VM  10u             /* the transport-encoding whose power init passes in a VM */

/* bluetoothd's transport names (0x100ba7c28) and driver names (0x100ba7c90), 24A446 */
static inline bool
obt_hci_name_in(const char *s, size_t n, const char *const *names)
{
	for (; *names; names++) {
		if (strlen(*names) == n && !strncmp(s, *names, n)) {
			return true;
		}
	}
	return false;
}

/* "TRANSPORT:DRIVER" or "TRANSPORT:DRIVER:/dev/..." with names bluetoothd knows; anything else makes it
 * log "FATAL ERROR - HCI Transport ... could not be matched" and start no stack. */
static inline bool
obt_hci_env_value_ok(const char *v)
{
	static const char *const transports[] = {
		"USB", "H4DS", "H4BC", "H4", "H5", "APPLEBT", "VIRT", "PCIE", "PCIESW", "PCIESUN", "APPLEPCIE", NULL,
	};
	static const char *const drivers[] = {
		"USB", "POSIX", "APPLEBT", "VIRT", "PCIE", "PCIESW", "PCIESUN", "APPLEPCIE", NULL,
	};
	const char *c1 = strchr(v, ':'), *c2;

	if (c1 == NULL || strlen(v) + sizeof(OBT_HCI_ENV_KEY) > OBT_HCI_ENV_MAX) {
		return false;
	}
	c2 = strchr(c1 + 1, ':');
	if (!obt_hci_name_in(v, (size_t)(c1 - v), transports) ||
	    !obt_hci_name_in(c1 + 1, c2 ? (size_t)(c2 - c1 - 1) : strlen(c1 + 1), drivers)) {
		return false;
	}
	return c2 == NULL || (c2[1] == '/' && strchr(c2 + 1, ':') == NULL && strchr(c2 + 1, ' ') == NULL);
}

/*
 * The boot argument oneios-bt-hci (arg: its value, "" when absent) -> the environment string for
 * bluetoothd in out (OBT_HCI_ENV_MAX bytes). Returns 1: on with arg; 2: on with the default (arg empty);
 * 3: on with the default because arg is not a value bluetoothd accepts; 0: off (arg "0": out empty, the
 * device tree's transport-encoding stays as xnuhost published it).
 */
static inline int
obt_hci_env(const char *arg, char *out, size_t n)
{
	int r = 1;

	if (n < OBT_HCI_ENV_MAX) {
		return 0;
	}
	out[0] = '\0';
	if (!strcmp(arg, "0")) {
		return 0;
	}
	if (arg[0] == '\0') {
		arg = OBT_HCI_ENV_DEFAULT;
		r = 2;
	} else if (!obt_hci_env_value_ok(arg)) {
		arg = OBT_HCI_ENV_DEFAULT;
		r = 3;
	}
	memcpy(out, OBT_HCI_ENV_KEY, sizeof(OBT_HCI_ENV_KEY) - 1);
	memcpy(out + sizeof(OBT_HCI_ENV_KEY) - 1, arg, strlen(arg) + 1);   /* fits: obt_hci_env_value_ok */
	return r;
}

#endif /* ONEIOS_BT_ENV_H */
