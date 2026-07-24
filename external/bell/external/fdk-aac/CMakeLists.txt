cmake_minimum_required(VERSION 3.16)

project(fdk-aac)

file(GLOB AACDEC_SRCS  "libAACdec/src/*.cpp")
file(GLOB AACENC_SRCS  "libAACenc/src/*.cpp")
file(GLOB PCMUTILS_SRCS "libPCMutils/src/*.cpp")
file(GLOB FDK_SRCS "libFDK/src/*.cpp")
file(GLOB SYS_SRCS "libSYS/src/*.cpp")
file(GLOB MPEGTPDEC_SRCS "libMpegTPDec/src/*.cpp")
file(GLOB MPEGTPENC_SRCS "libMpegTPEnc/src/*.cpp")
file(GLOB SBRDEC_SRCS "libSBRdec/src/*.cpp")
file(GLOB SBRENC_SRCS "libSBRenc/src/*.cpp")
file(GLOB ARITHCODING_SRCS "libArithCoding/src/*.cpp")
file(GLOB DRCDEC_SRCS "libDRCdec/src/*.cpp")
file(GLOB LIBSACDEC_SRCS "libSACdec/src/*.cpp")
file(GLOB LIBSACENC_SRCS "libSACenc/src/*.cpp")

set(AAC_SRCS
  ${AACDEC_SRCS}
  ${AACENC_SRCS}
  ${PCMUTILS_SRCS}
  ${FDK_SRCS}
  ${SYS_SRCS}
  ${MPEGTPDEC_SRCS}
  ${MPEGTPENC_SRCS}
  ${SBRDEC_SRCS}
  ${SBRENC_SRCS}
  ${ARITHCODING_SRCS}
  ${DRCDEC_SRCS}
  ${LIBSACDEC_SRCS}
  ${LIBSACENC_SRCS}
)

set(AAC_INCLUDES
    "libAACdec/include/"
    "libFDK/include"
    "libSBRdec/include/"
    "libSBRenc/include/"
    "libPCMutils/include/"
    "libSYS/include/"
    "libMpegTPDec/include/"
    "libMpegTPEnc/include/"
    "libAACenc/include/"
    "libArithCoding/include/"
    "libDRCdec/include"
    "libSACdec/include/"
    "libSACenc/include/"
)

add_library(fdk-aac ${AAC_SRCS})

### Library target includes
target_include_directories(fdk-aac
  PUBLIC
 ${AAC_INCLUDES})


target_compile_options(fdk-aac PRIVATE -fno-exceptions -Wno-unused-label -fno-rtti -Wno-maybe-uninitialized -Wno-deprecated-enum-enum-conversion -Wno-array-bounds -Wno-misleading-indentation -Wno-stringop-overflow -DUSE_DEFAULT_STDLIB)
