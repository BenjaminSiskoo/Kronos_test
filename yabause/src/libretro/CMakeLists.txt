# Kronos libretro core - CMake build.
#
# Same sources and definitions as Makefile.common (keep both lists in sync),
# except core/video/opengl/core/src/vidogl.c: the old OpenGL renderer is not
# used by the core (VIDCS only) and is not part of the standalone MSVC build.
# Lets Visual Studio build the core with MSVC: "File > Open > Folder" on
# yabause/src/libretro, or from a command prompt:
#
#   cmake -S yabause/src/libretro -B build_libretro -G "Visual Studio 17 2022" -A x64
#   cmake --build build_libretro --config Release
#
# It also works with gcc/clang and Ninja or make on Linux, and with MinGW.
# The output is kronos_libretro.dll / kronos_libretro.so / kronos_libretro.dylib.

cmake_minimum_required(VERSION 3.12)
project(kronos_libretro C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
get_filename_component(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(LIBRETRO_COMM_DIR ${CORE_DIR}/libretro-common)
set(LIBCHDR_DIR ${SOURCE_DIR}/tools/libchdr)
set(LZMA_DIR ${LIBCHDR_DIR}/deps/lzma-16.04)
set(FLAC_DIR ${LIBCHDR_DIR}/deps/flac-1.3.2)
set(ZLIB_DIR ${LIBCHDR_DIR}/deps/zlib-1.2.11)

option(KRONOS_LIBRETRO_BUNDLED_GLEXT
       "Use the GL/glext.h and KHR/khrplatform.h shipped in include/ (needed with MSVC, the Windows SDK has no glext.h)"
       ${MSVC})

#-------------------------------------------------------------------------------
# Musashi 68000 core: the opcode tables are generated at build time by m68kmake
# (what "make generate-files" does with the Makefile).
set(MUSASHI_SRC_DIR ${SOURCE_DIR}/core/m68k/musashi)
set(MUSASHI_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/musashi)
set(MUSASHI_GENERATED
  ${MUSASHI_GEN_DIR}/m68kops.h
  ${MUSASHI_GEN_DIR}/m68kops.c
  ${MUSASHI_GEN_DIR}/m68kopac.c
  ${MUSASHI_GEN_DIR}/m68kopdm.c
  ${MUSASHI_GEN_DIR}/m68kopnz.c)

add_executable(m68kmake ${MUSASHI_SRC_DIR}/m68kmake.c)
if(MSVC)
  target_compile_definitions(m68kmake PRIVATE _CRT_SECURE_NO_WARNINGS)
  # m68kmake built with /O2 by MSVC (VS 2022) writes every opcode mask and
  # match with bit 14 cleared (0xf000 -> 0xb000, 0xff00 -> 0xbf00). The
  # opcode table builder then never finds its 0xff00 group, runs past the end
  # of the table and the core hangs in YabauseInit(). The standalone build
  # compiles m68kmake as a separate, unoptimised project: do the same here.
  target_compile_options(m68kmake PRIVATE /Od)
endif()

# Refuse a broken generator output instead of building a core that hangs:
# the table must contain BRA.B (mask 0xff00, match 0x6000).
set(MUSASHI_CHECK_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/check_musashi.cmake)
file(WRITE ${MUSASHI_CHECK_SCRIPT} [=[
file(READ "${TABLE}" content)
string(FIND "${content}" "0xff00, 0x6000" pos)
if(pos EQUAL -1)
  file(REMOVE "${TABLE}")
  message(FATAL_ERROR "m68kmake produced a broken opcode table (${TABLE}): no BRA.B entry (mask 0xff00, match 0x6000)")
endif()
]=])

file(MAKE_DIRECTORY ${MUSASHI_GEN_DIR})
add_custom_command(
  OUTPUT ${MUSASHI_GENERATED}
  COMMAND m68kmake ${MUSASHI_GEN_DIR}/ ${MUSASHI_SRC_DIR}/m68k_in.c
  COMMAND ${CMAKE_COMMAND} -DTABLE=${MUSASHI_GEN_DIR}/m68kops.c -P ${MUSASHI_CHECK_SCRIPT}
  DEPENDS m68kmake ${MUSASHI_SRC_DIR}/m68k_in.c
  COMMENT "Generating the Musashi 68000 opcode tables"
  VERBATIM)
set_source_files_properties(${MUSASHI_GENERATED} PROPERTIES GENERATED ON)

#-------------------------------------------------------------------------------
# zlib: compiled from the sources like Makefile.common does. zlib's own
# CMakeLists renames zconf.h in the source tree when it is built out of
# source (the standalone build does that); use whichever copy is there.
set(ZLIB_CONF_DIR ${CMAKE_CURRENT_BINARY_DIR}/zlibconf)
if(EXISTS ${ZLIB_DIR}/zconf.h)
  configure_file(${ZLIB_DIR}/zconf.h ${ZLIB_CONF_DIR}/zconf.h COPYONLY)
elseif(EXISTS ${ZLIB_DIR}/zconf.h.included)
  configure_file(${ZLIB_DIR}/zconf.h.included ${ZLIB_CONF_DIR}/zconf.h COPYONLY)
else()
  message(FATAL_ERROR "zconf.h not found in ${ZLIB_DIR}")
endif()

#-------------------------------------------------------------------------------
set(KRONOS_SOURCES
  ${SOURCE_DIR}/core/osd/src/osdcore.c
  ${SOURCE_DIR}/sys/bios/src/bios.c
  ${SOURCE_DIR}/core/cd/libretro/cd-libretro.c
  ${SOURCE_DIR}/utils/src/cheat.c
  ${SOURCE_DIR}/utils/src/db.c
  ${SOURCE_DIR}/sys/memory/src/cs0.c
  ${SOURCE_DIR}/sys/memory/src/cs1.c
  ${SOURCE_DIR}/sys/memory/src/cs2.c
  ${SOURCE_DIR}/sys/memory/src/mpegcard.c
  ${SOURCE_DIR}/sys/memory/src/decrypt.c
  ${SOURCE_DIR}/sys/memory/src/eeprom.c
  ${SOURCE_DIR}/utils/src/error.c
  ${SOURCE_DIR}/utils/src/gameinfo.c
  ${SOURCE_DIR}/utils/src/japmodem.c
  ${SOURCE_DIR}/tools/junzip/junzip.c
  ${SOURCE_DIR}/sys/memory/src/memory.c
  ${SOURCE_DIR}/ctrl/src/movie.c
  ${SOURCE_DIR}/utils/src/netlink.c
  ${SOURCE_DIR}/core/peripheral/common/peripheral.c
  ${SOURCE_DIR}/sys/scsp/src/scspdsp.c
  ${SOURCE_DIR}/sys/scsp/src/scsp.c
  ${SOURCE_DIR}/sys/scsp/src/rax.c
  ${SOURCE_DIR}/sys/scsp/src/adsp2181.c
  ${SOURCE_DIR}/sys/scu/src/scu.c
  ${SOURCE_DIR}/sys/sh2/src/sh2core.c
  ${SOURCE_DIR}/sys/smpc/src/smpc.c
  ${SOURCE_DIR}/ctrl/src/stv.c
  ${SOURCE_DIR}/core/m68k/m68kcore.c
  ${SOURCE_DIR}/core/sh2/sh2_kronos/src/sh2int.c
  ${SOURCE_DIR}/core/sh2/sh2_kronos/src/sh2_opcodes.c
  ${SOURCE_DIR}/core/video/opengl/core/src/yglcache.c
  ${SOURCE_DIR}/core/video/opengl/compute_shader/src/vidcs.c
  ${SOURCE_DIR}/core/video/opengl/common/src/commongl.c
  ${SOURCE_DIR}/core/video/opengl/common/src/ogl_shader.c
  ${SOURCE_DIR}/core/video/opengl/compute_shader/src/yglcs.c
  ${SOURCE_DIR}/core/video/opengl/common/src/upscale_shader.c
  ${SOURCE_DIR}/core/sound/dummy/snddummy.c
  ${SOURCE_DIR}/sys/vdp1/src/vdp1.c
  ${SOURCE_DIR}/core/video/opengl/compute_shader/src/vdp1_compute.c
  ${SOURCE_DIR}/sys/vdp2/src/vdp2.c
  ${SOURCE_DIR}/core/video/opengl/common/src/common_glshader.c
  ${SOURCE_DIR}/core/video/opengl/common/src/vidshared.c
  ${SOURCE_DIR}/ctrl/src/yabause.c
  ${SOURCE_DIR}/core/video/opengl/common/src/rbg_compute.cpp
  ${MUSASHI_SRC_DIR}/m68kcpu.c
  ${SOURCE_DIR}/core/m68k/m68kmusashi.c
  ${MUSASHI_GENERATED}
  ${CORE_DIR}/libretro.c
  ${CORE_DIR}/glsym_private.c)

set(LIBRETRO_COMMON_SOURCES
  ${LIBRETRO_COMM_DIR}/compat/compat_posix_string.c
  ${LIBRETRO_COMM_DIR}/compat/compat_strl.c
  ${LIBRETRO_COMM_DIR}/compat/fopen_utf8.c
  ${LIBRETRO_COMM_DIR}/encodings/encoding_utf.c
  ${LIBRETRO_COMM_DIR}/file/file_path.c
  ${LIBRETRO_COMM_DIR}/file/file_path_io.c
  ${LIBRETRO_COMM_DIR}/compat/compat_strcasestr.c
  ${LIBRETRO_COMM_DIR}/string/stdstring.c
  ${LIBRETRO_COMM_DIR}/vfs/vfs_implementation.c
  ${LIBRETRO_COMM_DIR}/streams/file_stream.c
  ${LIBRETRO_COMM_DIR}/time/rtime.c
  ${LIBRETRO_COMM_DIR}/rthreads/rthreads.c
  ${LIBRETRO_COMM_DIR}/glsm/glsm.c
  ${LIBRETRO_COMM_DIR}/glsym/rglgen.c
  ${LIBRETRO_COMM_DIR}/glsym/glsym_gl.c)

set(CHD_SOURCES
  ${LIBCHDR_DIR}/src/chd.c
  ${LIBCHDR_DIR}/src/bitstream.c
  ${LIBCHDR_DIR}/src/huffman.c
  ${LIBCHDR_DIR}/src/flac.c
  ${LIBCHDR_DIR}/src/cdrom.c
  ${FLAC_DIR}/src/libFLAC/stream_decoder.c
  ${FLAC_DIR}/src/libFLAC/format.c
  ${FLAC_DIR}/src/libFLAC/bitreader.c
  ${FLAC_DIR}/src/libFLAC/crc.c
  ${FLAC_DIR}/src/libFLAC/memory.c
  ${FLAC_DIR}/src/libFLAC/lpc.c
  ${FLAC_DIR}/src/libFLAC/cpu.c
  ${FLAC_DIR}/src/libFLAC/md5.c
  ${FLAC_DIR}/src/libFLAC/fixed.c
  ${LZMA_DIR}/C/LzmaEnc.c
  ${LZMA_DIR}/C/LzFind.c
  ${LZMA_DIR}/C/LzmaDec.c
  ${ZLIB_DIR}/adler32.c
  ${ZLIB_DIR}/crc32.c
  ${ZLIB_DIR}/inffast.c
  ${ZLIB_DIR}/inflate.c
  ${ZLIB_DIR}/inftrees.c
  ${ZLIB_DIR}/zutil.c)

if(WIN32)
  list(APPEND KRONOS_SOURCES ${SOURCE_DIR}/ctrl/threads/thr-windows.c)
  list(APPEND CHD_SOURCES ${FLAC_DIR}/src/libFLAC/windows_unicode_filenames.c)
elseif(APPLE)
  list(APPEND KRONOS_SOURCES
    ${SOURCE_DIR}/ctrl/threads/thr-linux.c
    ${SOURCE_DIR}/ctrl/threads/linux/pthread_barrier.c)
else()
  list(APPEND KRONOS_SOURCES ${SOURCE_DIR}/ctrl/threads/thr-linux.c)
endif()

add_library(kronos_libretro SHARED
  ${KRONOS_SOURCES} ${LIBRETRO_COMMON_SOURCES} ${CHD_SOURCES})
set_target_properties(kronos_libretro PROPERTIES PREFIX "" OUTPUT_NAME kronos_libretro)

#-------------------------------------------------------------------------------
set(KRONOS_INCLUDE_DIRS
  ${MUSASHI_GEN_DIR}
  ${CORE_DIR}
  ${SOURCE_DIR}
  ${SOURCE_DIR}/sys/bios/include
  ${SOURCE_DIR}/sys/vdp1/include
  ${SOURCE_DIR}/sys/vdp2/include
  ${SOURCE_DIR}/sys/sh2/include
  ${SOURCE_DIR}/sys/memory/include
  ${SOURCE_DIR}/sys/smpc/include
  ${SOURCE_DIR}/sys/scsp/include
  ${SOURCE_DIR}/sys/scu/include
  ${SOURCE_DIR}/core/video/opengl/core/include
  ${SOURCE_DIR}/core/video/opengl/compute_shader/include
  ${SOURCE_DIR}/core/video/opengl/common/include
  ${SOURCE_DIR}/core/video/soft
  ${SOURCE_DIR}/core/sh2/sh2_kronos/include
  ${SOURCE_DIR}/core/peripheral/common
  ${SOURCE_DIR}/core/net
  ${SOURCE_DIR}/core/net/linux
  ${SOURCE_DIR}/core/net/windows
  ${SOURCE_DIR}/core/m68k
  ${SOURCE_DIR}/core/osd/include
  ${SOURCE_DIR}/utils/include
  ${SOURCE_DIR}/tools/junzip
  ${SOURCE_DIR}/ctrl/include
  ${SOURCE_DIR}/ctrl/threads
  ${SOURCE_DIR}/ctrl/threads/linux
  ${LIBRETRO_COMM_DIR}/include
  ${MUSASHI_SRC_DIR}
  ${LIBCHDR_DIR}/src
  ${LZMA_DIR}/C
  ${LIBCHDR_DIR}/deps/crypto
  ${FLAC_DIR}/include
  ${FLAC_DIR}/src/libFLAC/include
  ${ZLIB_CONF_DIR}
  ${ZLIB_DIR})
if(KRONOS_LIBRETRO_BUNDLED_GLEXT)
  list(INSERT KRONOS_INCLUDE_DIRS 0 ${CORE_DIR}/include)
endif()
target_include_directories(kronos_libretro PRIVATE ${KRONOS_INCLUDE_DIRS})

target_compile_definitions(kronos_libretro PRIVATE
  __LIBRETRO__ HAVE_LIBGL VERSION="2.8.0" HAVE_LROUND _7ZIP_ST
  FLAC__HAS_OGG=0 FLAC__NO_DLL NO_CLI HAVE_C99_VARIADIC_MACROS=1
  HAVE_FLOORF=1 HAVE_STDINT_H=1 IMPROVED_SAVESTATES HAVE_GLSYM_PRIVATE
  CELL_ASYNC=1 HAVE_THREADS=1 USE_CACHE=1 _OGL3_)

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                  WORKING_DIRECTORY ${CORE_DIR}
                  OUTPUT_VARIABLE KRONOS_GIT_VERSION
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(KRONOS_GIT_VERSION)
    target_compile_definitions(kronos_libretro PRIVATE GIT_VERSION=" ${KRONOS_GIT_VERSION}")
  endif()
endif()

if(MSVC)
  # wd4996: POSIX names (strdup...), wd4068: unknown pragma (#pragma mark)
  target_compile_definitions(kronos_libretro PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
  target_compile_options(kronos_libretro PRIVATE /bigobj /wd4996 /wd4068 /MP)
  # windows.h defines min/max macros, which break std::min/std::max in C++
  set_source_files_properties(${SOURCE_DIR}/core/video/opengl/common/src/rbg_compute.cpp
                              PROPERTIES COMPILE_DEFINITIONS NOMINMAX)
else()
  target_compile_definitions(kronos_libretro PRIVATE
    HAVE_BUILTIN_BSWAP16=1 HAVE_BUILTIN_BSWAP32=1
    HAVE_GETTIMEOFDAY=1 HAVE_SYS_TIME_H=1)
  set_source_files_properties(${KRONOS_SOURCES} ${LIBRETRO_COMMON_SOURCES} ${CHD_SOURCES}
                              PROPERTIES COMPILE_OPTIONS "-Wno-unused-value")
  set_property(TARGET kronos_libretro PROPERTY C_STANDARD 17)
  set_property(TARGET kronos_libretro PROPERTY C_EXTENSIONS ON)
  set_property(TARGET kronos_libretro PROPERTY CXX_STANDARD 17)
  set_property(TARGET kronos_libretro PROPERTY CXX_EXTENSIONS ON)
endif()

if(WIN32)
  target_compile_definitions(kronos_libretro PRIVATE _WINDOWS)
  target_link_libraries(kronos_libretro PRIVATE opengl32 winmm)
  if(MINGW)
    # FLAC's share/compat.h: MinGW has fseeko/ftello (Makefile: HAVE_FSEEKO = 1)
    target_compile_definitions(kronos_libretro PRIVATE HAVE_FSEEKO)
    target_link_options(kronos_libretro PRIVATE -static-libgcc -static-libstdc++)
  endif()
elseif(APPLE)
  target_compile_definitions(kronos_libretro PRIVATE ARCH_IS_MACOSX=1)
  find_package(OpenGL REQUIRED)
  target_link_libraries(kronos_libretro PRIVATE ${OPENGL_LIBRARIES} pthread)
else()
  target_compile_definitions(kronos_libretro PRIVATE ARCH_IS_LINUX=1 HAVE_SYS_PARAM_H)
  set_target_properties(kronos_libretro PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_options(kronos_libretro PRIVATE -Wl,--no-undefined -Wl,--version-script=${CORE_DIR}/link.T)
  target_link_libraries(kronos_libretro PRIVATE GL pthread atomic)
endif()
