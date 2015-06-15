SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_SYSTEM_VERSION 1)
# specify the cross compiler
SET(CMAKE_C_COMPILER $ENV{HOME}/raspberrypi/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian/bin/arm-linux-gnueabihf-gcc)
SET(CMAKE_CXX_COMPILER $ENV{HOME}/raspberrypi/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian/bin/arm-linux-gnueabihf-g++)

SET(CMAKE_FIND_ROOT_PATH $ENV{HOME}/raspberrypi/rootfs)
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)


# where is the target environment
#SET(CMAKE_SYSROOT "/home/evan/raspberrypi/rootfs")
#SET(CMAKE_SYSTEM_PREFIX_PATH "/home/evan/raspberrypi/rootfs")
#SET(CMAKE_SYSTEM_INCLUDE_PATH /include "$ENV{HOME}/raspberrypi/rootfs/include")
#SET(CMAKE_SYSTEM_LIBRARY_PATH /lib "$ENV{HOME}/raspberrypi/rootfs/lib")
#SET(CMAKE_SYSTEM_PROGRAM_PATH /bin "$ENV{HOME}/raspberrypi/rootfs/bin")

# search for programs in the build host directories
#SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# for libraries and headers in the target directories
#SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
#SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

#SET(CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES "$ENV{HOME}/raspberrypi/rootfs/include")
