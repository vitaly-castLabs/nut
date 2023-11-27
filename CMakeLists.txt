cmake_minimum_required(VERSION 3.12)
project(nututils)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -Wall -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64")

# libnut
set(LIBNUT_SRC
    src/libnut/muxer.c
    src/libnut/demuxer.c
    src/libnut/reorder.c
    src/libnut/framecode.c
)

add_library(nut STATIC ${LIBNUT_SRC})
target_include_directories(nut PUBLIC ${PROJECT_SOURCE_DIR}/src/libnut)

# nututils
add_executable(nutindex src/nututils/nutindex.c)
add_executable(nutparse src/nututils/nutparse.c)

add_executable(remux_avc src/nututils/remux_avc.c)
target_link_libraries(remux_avc nut)

set(NUTMERGE_SRC
    src/nututils/nutmerge.c
    src/nututils/demux_avi.c
    src/nututils/demux_ogg.c
    src/nututils/framer_mp3.c
    src/nututils/framer_mpeg4.c
    src/nututils/framer_vorbis.c
)
add_executable(nutmerge ${NUTMERGE_SRC})
target_link_libraries(nutmerge nut)
