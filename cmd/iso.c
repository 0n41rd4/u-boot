// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cmd/iso.c -- ISO handling command
 *
 */

#include <blk.h>
#include <command.h>
#include <malloc.h>
#include <part.h>
#include <vsprintf.h>

static int do_iso(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	int dev = 0;
	char *ep;
	struct blk_desc *blk_dev_desc = NULL;
	struct part_driver *drv = NULL;

	if (argc != 3)
		return CMD_RET_USAGE;

	dev = (int)dectoul(argv[2], &ep);
	if (!ep || ep[0] != '\0') {
		printf("'%s' is not a number\n", argv[3]);
		return CMD_RET_USAGE;
	}
	blk_dev_desc = blk_get_dev(argv[1], dev);
	if (!blk_dev_desc) {
		printf("%s: %s dev %d NOT available\n",
		       __func__, argv[1], dev);
		return CMD_RET_FAILURE;
	}

	printf("ISO: \n");
	drv = part_driver_get_type(PART_TYPE_ISO);
	if (!drv) {
		printf("ISO driver not found\n");
	}
	if (drv->test && drv->test(blk_dev_desc)) {
		printf("ISO not recognized\n");
		return CMD_RET_FAILURE;
	}
	if (drv->print )
		drv->print(blk_dev_desc);

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(iso, CONFIG_SYS_MAXARGS, 1, do_iso,
	"print ISO",
	"<interface> <dev>\n"
	" - print ISO in the given block interface\n"
	" Example usage:\n"
	" iso mmc 0\n"
);
