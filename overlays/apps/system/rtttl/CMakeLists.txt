# ##############################################################################
# apps/system/rtttl/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# ##############################################################################

if(CONFIG_SYSTEM_RTTTL)
  nuttx_add_application(
    MODULE
    ${CONFIG_SYSTEM_RTTTL}
    NAME
    ${CONFIG_SYSTEM_RTTTL_PROGNAME}
    STACKSIZE
    ${CONFIG_SYSTEM_RTTTL_STACKSIZE}
    PRIORITY
    ${CONFIG_SYSTEM_RTTTL_PRIORITY}
    SRCS
    rtttl_main.c)
endif()
