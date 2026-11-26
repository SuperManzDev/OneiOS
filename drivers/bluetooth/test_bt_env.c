/*
 * Host test: bluetoothd's BT_HCI_TRANSPORT (oneios_bt_env.h) and the vendor-command answers of the H4 core
 * (oneios_bt_h4.c obt_local_answer) in both modes. Run by drivers/tests/host.sh.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "oneios_bt.h"
#include "oneios_bt_env.h"

bool obt_vsc_empty;
char oneios_bluetoothd_env[64];

static void
env(void)
{
	char out[OBT_HCI_ENV_MAX];

	assert(obt_hci_env("", out, sizeof(out)) == 2 && !strcmp(out, "BT_HCI_TRANSPORT=H4:POSIX"));
	assert(obt_hci_env("0", out, sizeof(out)) == 0 && out[0] == '\0');
	assert(obt_hci_env("H4BC:POSIX", out, sizeof(out)) == 1 && !strcmp(out, "BT_HCI_TRANSPORT=H4BC:POSIX"));
	assert(obt_hci_env("H5:POSIX:/dev/cu.bluetooth", out, sizeof(out)) == 1 &&
	    !strcmp(out, "BT_HCI_TRANSPORT=H5:POSIX:/dev/cu.bluetooth"));
	/* bluetoothd's own lookup would fail on these: the default instead */
	assert(obt_hci_env("H4", out, sizeof(out)) == 3 && !strcmp(out, "BT_HCI_TRANSPORT=H4:POSIX"));
	assert(obt_hci_env("H4,POSIX", out, sizeof(out)) == 3);
	assert(obt_hci_env("H4:UART", out, sizeof(out)) == 3);
	assert(obt_hci_env("H:POSIX", out, sizeof(out)) == 3);
	assert(obt_hci_env("H4:POSIX:dev", out, sizeof(out)) == 3);
	assert(obt_hci_env("H4:POSIX:/dev/a:b", out, sizeof(out)) == 3);
	assert(obt_hci_env("H4:POSIX:/dev/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", out, sizeof(out)) == 3);
	assert(obt_hci_env("", out, 16) == 0);                 /* no room: nothing */
	/* the longest value that fits leaves the NUL in the buffer */
	{
		char v[64] = "H4:POSIX:/";
		while (strlen(v) + sizeof(OBT_HCI_ENV_KEY) < OBT_HCI_ENV_MAX) {
			strcat(v, "a");
		}
		assert(obt_hci_env(v, out, sizeof(out)) == 1 && strlen(out) == OBT_HCI_ENV_MAX - 1);
	}
}

static void
vsc(void)
{
	struct obt_shim s = { 0 };
	uint8_t ev[OBT_LOCAL_EVENT_MAX];
	const uint8_t wr_addr[] = { 1, 0x06, 0xfc, 6, 1, 2, 3, 4, 5, 6 };   /* 0xfc06: BTVC's Write BD_ADDR */
	const uint8_t unknown[] = { 1, 0x99, 0xfd, 1, 7 };                   /* 0xfd99: in no table */
	const uint8_t reset[] = { 1, 0x03, 0x0c, 0 };

	/* iOS 16's table: unknown vendor commands get 0x01 and 8 bytes */
	assert(obt_local_answer(&s, unknown, sizeof(unknown), ev) == 15 && ev[1] == 0x0e && ev[2] == 12 && ev[6] == 0x01 &&
	    ev[7] == 7);
	assert(obt_local_answer(&s, reset, sizeof(reset), ev) == 0);
	/* bluetoothd's virtual controller: every vendor command Command Complete, status 0, no parameters */
	s.vsc_empty = true;
	assert(obt_local_answer(&s, unknown, sizeof(unknown), ev) == 7);
	assert(ev[0] == 4 && ev[1] == 0x0e && ev[2] == 4 && ev[3] == 1 && ev[4] == 0x99 && ev[5] == 0xfd && ev[6] == 0);
	assert(obt_local_answer(&s, wr_addr, sizeof(wr_addr), ev) == 7 && ev[6] == 0 && ev[4] == 0x06 && ev[5] == 0xfc);
	assert(obt_local_answer(&s, reset, sizeof(reset), ev) == 0);        /* standard commands go to the controller */
	{
		const uint8_t ext[] = { 1, 0x4a, 0x20, 0 };                   /* 0x204a stays answered here */
		assert(obt_local_answer(&s, ext, sizeof(ext), ev) == 8);
	}
}

int
main(void)
{
	env();
	vsc();
	printf("drivers: bluetooth env and vendor answers ok\n");
	return 0;
}
