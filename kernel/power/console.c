
#include <linux/vt_kern.h>
#include <linux/kbd_kern.h>
#include <linux/vt.h>
#include <linux/module.h>
#include "power.h"

#define SUSPEND_CONSOLE	(MAX_NR_CONSOLES-1)

static int orig_fgconsole, orig_kmsg;

int pm_prepare_console(void)
{
	orig_fgconsole = vt_move_to_console(SUSPEND_CONSOLE, 1);
	if (orig_fgconsole < 0)
		return 1;

	orig_kmsg = vt_kmsg_redirect(SUSPEND_CONSOLE);
	return 0;
}

void pm_restore_console(void)
{
	if (orig_fgconsole >= 0) {
		vt_move_to_console(orig_fgconsole, 0);
		vt_kmsg_redirect(orig_kmsg);
	}
}
