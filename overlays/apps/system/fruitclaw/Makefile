############################################################################
# apps/system/fruitclaw/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)$(DELIM)system$(DELIM)fruitclaw$(DELIM)include

ifeq ($(CONFIG_FRUITCLAW_ENABLE_BERRY),y)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)$(DELIM)interpreters$(DELIM)berry$(DELIM)include
endif

ifeq ($(CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD),y)
CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)$(DELIM)arch$(DELIM)arm$(DELIM)src$(DELIM)rp23xx
CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)$(DELIM)arch$(DELIM)arm$(DELIM)src$(DELIM)common
endif

PROGNAME  = $(CONFIG_FRUITCLAW_PROGNAME)
PRIORITY  = $(CONFIG_FRUITCLAW_PRIORITY)
STACKSIZE = $(CONFIG_FRUITCLAW_STACKSIZE)
MODULE    = $(CONFIG_SYSTEM_FRUITCLAW)

MAINSRC = fruitclaw_main.c

CSRCS  = core/fc_config.c
CSRCS += core/fc_progress.c
CSRCS += core/fc_guard.c
CSRCS += core/fc_queue.c
CSRCS += core/fc_cap.c
CSRCS += core/fc_router.c
CSRCS += core/fc_selftest.c
CSRCS += storage/fc_memory.c
CSRCS += sched/fc_scheduler.c
CSRCS += net/fc_http.c
CSRCS += net/fc_web.c
CSRCS += net/fc_telegram.c
CSRCS += net/fc_deepseek.c
ifeq ($(CONFIG_FRUITCLAW_MCP_SERVER),y)
CSRCS += net/fc_mcp.c
endif
CSRCS += core/fc_agent.c
CSRCS += script/fc_berry.c
CSRCS += caps/fc_caps_builtin.c

include $(APPDIR)/Application.mk
