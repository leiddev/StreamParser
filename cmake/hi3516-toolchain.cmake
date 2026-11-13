# Hi3516CV500/DV300 cross build: Cortex-A7, armv7, glibc, softfp.
#
# The slim arm-himix200 toolchain only ships the softfp ABI variant and finds
# its own sysroot and multilib (a7_softfp_neon-vfpv4) by itself, so nothing
# here sets CMAKE_SYSROOT: doing so would hide that multilib.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7-a)

# Override on the command line when the toolchain lives elsewhere:
#   -DHI3516_TOOLCHAIN_DIR=/path/to/arm-himix200-linux
if(NOT HI3516_TOOLCHAIN_DIR)
    set(HI3516_TOOLCHAIN_DIR /opt/hisi-linux/x86-arm/arm-himix200-linux)
endif()
set(TOOLCHAIN_DIR ${HI3516_TOOLCHAIN_DIR})

set(CMAKE_C_COMPILER   ${TOOLCHAIN_DIR}/bin/arm-himix200-linux-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_DIR}/bin/arm-himix200-linux-g++)

set(CMAKE_C_FLAGS_INIT   "-mcpu=cortex-a7 -mfloat-abi=softfp -mfpu=neon-vfpv4")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a7 -mfloat-abi=softfp -mfpu=neon-vfpv4")

set(CMAKE_FIND_ROOT_PATH ${TOOLCHAIN_DIR})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
