# ##############################################################################
# apps/system/bootguard/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# ##############################################################################

if(CONFIG_SYSTEM_BOOTGUARD)
  nuttx_add_application(
    MODULE
    ${CONFIG_SYSTEM_BOOTGUARD}
    NAME
    ${CONFIG_SYSTEM_BOOTGUARD_PROGNAME}
    STACKSIZE
    ${CONFIG_SYSTEM_BOOTGUARD_STACKSIZE}
    PRIORITY
    ${CONFIG_SYSTEM_BOOTGUARD_PRIORITY}
    SRCS
    bootguard_main.c)
endif()
