# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

include(CPM)

# define the cpu architecture for threadx
set(THREADX_ARCH "cortex_m7")
set(THREADX_TOOLCHAIN "gnu")

# nx_cloud
CPMAddPackage(
    NAME azure_iot_preview
    GIT_REPOSITORY https://github.com/azure-rtos/azure-iot-preview
    GIT_TAG master
    DOWNLOAD_ONLY
)
if (azure_iot_preview_ADDED)
    add_subdirectory(${azure_iot_preview_SOURCE_DIR}/nx_cloud ${CMAKE_CURRENT_BINARY_DIR}/nx_cloud)
endif()

# threadX
set(TX_USER_FILE "${CMAKE_CURRENT_LIST_DIR}/threadx/tx_user.h" CACHE STRING "Enable TX user configuration")
CPMAddPackage(
    NAME threadx
    GIT_REPOSITORY https://github.com/azure-rtos/threadx.git
    GIT_TAG master
)

# netX-duo
set(NX_USER_FILE "${CMAKE_CURRENT_LIST_DIR}/netxduo/nx_user.h" CACHE STRING "Enable NX user configuration")
set(NXD_ENABLE_FILE_SERVERS OFF CACHE BOOL "Disable fileX dependency by netxduo")
CPMAddPackage(
    NAME netxduo
    GIT_REPOSITORY https://github.com/azure-rtos/netxduo.git
    GIT_TAG master
)

add_subdirectory(netx_driver)
add_subdirectory(MIMXRT1060-evk)
