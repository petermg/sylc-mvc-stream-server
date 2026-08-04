cmake_minimum_required(VERSION 3.20)
project(sylc_mvc_stream_engine LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(edge264 STATIC src/edge264/src/edge264.c)
target_include_directories(edge264 PUBLIC src/edge264)
target_compile_options(edge264 PRIVATE
  -O3 -march=native -mtune=native -flto
  -Wall -Wextra -Wno-format -Wno-unused-parameter -Wno-unused-function
  -Wno-missing-field-initializers -Wno-psabi -Wno-sign-compare -flax-vector-conversions)
set_target_properties(edge264 PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)

add_executable(sylc_hsbs_pipe src/sylc_hsbs_pipe.cpp)
target_compile_options(sylc_hsbs_pipe PRIVATE -O3 -march=native -mtune=native -flto -Wall -Wextra)
target_link_libraries(sylc_hsbs_pipe PRIVATE edge264 Threads::Threads m)
set_target_properties(sylc_hsbs_pipe PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)

set(UDF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src/videolan/libudfread-1.2.0")
set(UDF_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${UDF_GENERATED}/udfread")
set(UDFREAD_VERSION_MAJOR 1)
set(UDFREAD_VERSION_MINOR 2)
set(UDFREAD_VERSION_MICRO 0)
configure_file("${UDF_ROOT}/src/udfread-version.h.in" "${UDF_GENERATED}/udfread-version.h" @ONLY)
configure_file("${UDF_ROOT}/src/udfread.h" "${UDF_GENERATED}/udfread/udfread.h" COPYONLY)
configure_file("${UDF_ROOT}/src/blockinput.h" "${UDF_GENERATED}/udfread/blockinput.h" COPYONLY)
configure_file("${UDF_ROOT}/src/attributes.h" "${UDF_GENERATED}/udfread/attributes.h" COPYONLY)

add_library(udfread STATIC
  "${UDF_ROOT}/src/default_blockinput.c"
  "${UDF_ROOT}/src/ecma167.c"
  "${UDF_ROOT}/src/udfread.c"
  "${UDF_ROOT}/src/udfread-version.c")
target_include_directories(udfread PUBLIC "${UDF_GENERATED}" PRIVATE "${UDF_ROOT}" "${UDF_ROOT}/src")
target_compile_definitions(udfread PRIVATE
  HAVE_CONFIG_H=0
  UDFREAD_API_EXPORT
  _ISOC99_SOURCE
  _POSIX_C_SOURCE=200809L
  HAVE_UNISTD_H=1
  HAVE_FCNTL_H=1
  HAVE_SYS_TYPES_H=1
  HAVE_INTTYPES_H=1
  HAVE_STDINT_H=1)
target_compile_options(udfread PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

add_executable(sylc_iso_source
  src/iso/sylc_iso_source.cpp
  src/iso/sylc_iso_feature_posix.cpp
  src/iso/sylc_libbluray_probe.cpp
  src/iso/sylc_clpi_audio_seek.cpp
  src/iso/sylc_m2ts_audio_demuxer.cpp
  src/iso/sylc_m2ts_pgs_demuxer.cpp
  src/sylc_m2ts/src/m2ts_reader.cpp
  src/sylc_m2ts/src/h264_nal_parser.cpp
  src/sylc_m2ts/src/mvc_m2ts_demuxer.cpp
  src/sylc_m2ts/src/ssif_parser.cpp
  src/sylc_m2ts/src/mvc_ssif_demuxer.cpp)
target_include_directories(sylc_iso_source PRIVATE
  src/iso
  src/sylc_m2ts/include
  "${UDF_GENERATED}")
target_compile_options(sylc_iso_source PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(sylc_iso_source PRIVATE udfread Threads::Threads m dl)

add_executable(sylc_truehd_m2ts_extract_test
  src/iso/truehd_m2ts_extract_test.cpp
  src/iso/sylc_m2ts_audio_demuxer.cpp
  src/sylc_m2ts/src/m2ts_reader.cpp)
target_include_directories(sylc_truehd_m2ts_extract_test PRIVATE src/iso src/sylc_m2ts/include)
target_compile_options(sylc_truehd_m2ts_extract_test PRIVATE -O2 -Wall -Wextra)


add_executable(sylc_pgs_m2ts_extract_test
  src/iso/pgs_m2ts_extract_test.cpp
  src/iso/sylc_m2ts_pgs_demuxer.cpp
  src/sylc_m2ts/src/m2ts_reader.cpp)
target_include_directories(sylc_pgs_m2ts_extract_test PRIVATE src/iso src/sylc_m2ts/include)
target_compile_options(sylc_pgs_m2ts_extract_test PRIVATE -O2 -Wall -Wextra)

add_executable(sylc_recovery_logic_test src/iso/recovery_logic_test.cpp)
target_include_directories(sylc_recovery_logic_test PRIVATE src/iso src/sylc_m2ts/include)
target_compile_options(sylc_recovery_logic_test PRIVATE -O2 -Wall -Wextra)

add_executable(sylc_truehd_framing_test src/iso/truehd_framing_test.cpp)
target_include_directories(sylc_truehd_framing_test PRIVATE src/iso)
target_compile_options(sylc_truehd_framing_test PRIVATE -O2 -Wall -Wextra)

enable_testing()
add_test(NAME mvc_recovery_logic COMMAND sylc_recovery_logic_test)
add_test(NAME truehd_framing COMMAND sylc_truehd_framing_test)
