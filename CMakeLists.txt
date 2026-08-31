cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBOPUSENC_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/enc_opus.c
)

SET(LIBOPUSENC_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libopus.a
)

SET(LIBOPUSENC_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libopusenc
        "${LIBOPUSENC_SRC}"
        "${LIBOPUSENC_LIB}"
        []
        ""
        "${LIBOPUSENC_INC}"
        ""
        "1")
