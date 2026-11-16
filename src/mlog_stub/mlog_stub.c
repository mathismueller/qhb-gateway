// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Stub exports for hisi_spi2tty mlog hooks");

#define MLOG_TTY_NAME_SIZE 128
char mlog_tty_name[MLOG_TTY_NAME_SIZE + 1] = {0};
EXPORT_SYMBOL(mlog_tty_name);

int g_iTTYInCount = 0;
EXPORT_SYMBOL(g_iTTYInCount);

int g_iTTYOutCount = 0;
EXPORT_SYMBOL(g_iTTYOutCount);

int g_iStartTtyLog = 0;
EXPORT_SYMBOL(g_iStartTtyLog);

int g_iLenthTtyLog = 0;
EXPORT_SYMBOL(g_iLenthTtyLog);

#define TTY_LOG_ITEM 32
#define MAXITEM_TTY_LOG (1024 * 1024 / TTY_LOG_ITEM)
unsigned char g_acTtyLog[MAXITEM_TTY_LOG][TTY_LOG_ITEM];
EXPORT_SYMBOL(g_acTtyLog);

void mlog_set_tty_name(const char *name)
{
    g_iTTYInCount = 0;
    g_iTTYOutCount = 0;
    if (name)
        snprintf(mlog_tty_name, MLOG_TTY_NAME_SIZE, "%s", name);
}
EXPORT_SYMBOL(mlog_set_tty_name);

char *mlog_get_tty_name(void)
{
    return mlog_tty_name;
}
EXPORT_SYMBOL(mlog_get_tty_name);

static int __init mlog_stub_init(void)
{
    pr_info("mlog_stub: loaded mlog symbol provider\n");
    return 0;
}

static void __exit mlog_stub_exit(void)
{
    pr_info("mlog_stub: unloaded\n");
}

module_init(mlog_stub_init);
module_exit(mlog_stub_exit);
