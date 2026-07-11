# ##############################################################################
# apps/system/piousbhost/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# ##############################################################################

if(CONFIG_SYSTEM_PIOUSBHOST)
  nuttx_add_application(
    MODULE
    ${CONFIG_SYSTEM_PIOUSBHOST}
    NAME
    ${CONFIG_SYSTEM_PIOUSBHOST_PROGNAME}
    STACKSIZE
    ${CONFIG_SYSTEM_PIOUSBHOST_STACKSIZE}
    PRIORITY
    ${CONFIG_SYSTEM_PIOUSBHOST_PRIORITY}
    SRCS
    piousbhost_main.c)
endif()
