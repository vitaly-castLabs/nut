cmake_minimum_required(VERSION 3.12)
project(nututils)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -Wall -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64")

# libnut
set(LIBNUT_OBJS
    src/libnut/muxer.c
    src/libnut/demuxer.c
    src/libnut/reorder.c
    src/libnut/framecode.c
)

add_library(libnut STATIC ${LIBNUT_OBJS})
target_include_directories(libnut PUBLIC src/libnut)

# nututils
set(NUTUTILS_PROGS
    src/nututils/nutmerge
    src/nututils/nutindex
    src/nututils/nutparse
    src/nututils/remux_avc
)

set(NUTMERGE_OBJS
    src/nututils/nutmerge.c
    src/nututils/demux_avi.c
    src/nututils/demux_ogg.c
    src/nututils/framer_mp3.c
    src/nututils/framer_mpeg4.c
    src/nututils/framer_vorbis.c
)

set(REMUX_AVC_OBJS
    src/nututils/remux_avc.c
)

add_executable(nutmerge ${NUTMERGE_OBJS})
target_link_libraries(nutmerge libnut)

add_executable(remux_avc ${REMUX_AVC_OBJS})
target_link_libraries(remux_avc libnut)

# install targets
install(TARGETS libnut
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
)

install(FILES src/libnut/libnut.h DESTINATION include)

install(TARGETS nutmerge remux_avc
        RUNTIME DESTINATION bin
)

# uninstall target
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake
)

# clean targets
add_custom_target(clean-all
    COMMAND ${CMAKE_BUILD_TOOL} clean
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clean-all.cmake
)

# Add dependencies for install and uninstall targets
add_dependencies(uninstall clean-all)
