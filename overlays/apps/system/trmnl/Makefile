############################################################################
# apps/system/trmnl/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)$(DELIM)system$(DELIM)zlib$(DELIM)zlib

PROGNAME  = $(CONFIG_SYSTEM_TRMNL_PROGNAME)
PRIORITY  = $(CONFIG_SYSTEM_TRMNL_PRIORITY)
STACKSIZE = $(CONFIG_SYSTEM_TRMNL_STACKSIZE)
MODULE    = $(CONFIG_SYSTEM_TRMNL)

MAINSRC = trmnl_main.c

include $(APPDIR)/Application.mk
