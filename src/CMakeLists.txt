#
# Set compile flags for entire src/ directory
#
enable_tnt_compile_flags()

if (NOT TARGET_OS_DARWIN)
    add_subdirectory(module)
endif()

include_directories(${LIBEV_INCLUDE_DIR})
include_directories(${LIBEIO_INCLUDE_DIR})
include_directories(${LIBCORO_INCLUDE_DIR})
include_directories(${LIBGOPT_INCLUDE_DIR})
include_directories(${READLINE_INCLUDE_DIR})

# Compile src/lua/*.lua files into src/lua/*.lua.c sources
set(lua_sources)
lua_source(lua_sources lua/init.lua)
lua_source(lua_sources lua/fiber.lua)
lua_source(lua_sources lua/uuid.lua)
lua_source(lua_sources lua/digest.lua)
lua_source(lua_sources lua/msgpackffi.lua)
lua_source(lua_sources lua/uri.lua)
lua_source(lua_sources lua/console.lua)
lua_source(lua_sources lua/bsdsocket.lua)
lua_source(lua_sources lua/errno.lua)
lua_source(lua_sources lua/log.lua)
lua_source(lua_sources lua/box_net_box.lua)
lua_source(lua_sources lua/help.lua)
lua_source(lua_sources lua/help_en_US.lua)
lua_source(lua_sources lua/tap.lua)
lua_source(lua_sources lua/fio.lua)
lua_source(lua_sources ../third_party/luafun/fun.lua)

add_custom_target(generate_lua_sources
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/src/box
    DEPENDS ${lua_sources})
set_property(DIRECTORY PROPERTY ADDITIONAL_MAKE_CLEAN_FILES ${lua_sources})

add_custom_target(ragel
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMAND ragel -G2 src/uri.rl -o src/uri.c)

set (core_sources
     say.cc
     memory.cc
     fiber.cc
     exception.cc
     coro.cc
     object.cc
     assoc.c
 )

add_library(core STATIC ${core_sources})
target_link_libraries(core
    salad small pthread
    ${LIBEV_LIBRARIES}
    ${LIBCORO_LIBRARIES}
)

set (server_sources
     util.cc
     find_path.c
     sio.cc
     evio.cc
     coio.cc
     coeio.cc
     iobuf.cc
     coio_buf.cc
     pickle.cc
     stat.cc
     ipc.cc
     errinj.cc
     fio.c
     crc32.c
     random.c
     scramble.c
     opts.c
     cfg.cc
     cpu_feature.c
     fiob.c
     tt_uuid.c
     ffisyms.cc
     uri.c
     coeio_file.cc
     lua/digest.cc
     lua/init.cc
     lua/fiber.cc
     lua/trigger.cc
     lua/ipc.cc
     lua/msgpack.cc
     lua/utils.cc
     lua/errno.c
     lua/bsdsocket.cc
     lua/pickle.cc
     lua/fio.cc
     ${lua_sources}
)

if (ENABLE_TRACE)
    set (server_sources ${server_sources} trace.c)
endif()

set_source_files_compile_flags(${server_sources})
add_library(server STATIC ${server_sources})
target_link_libraries(server core)

set (common_libraries server core misc bitset msgpuck)

list(APPEND common_libraries
    ${LIBEIO_LIBRARIES}
    ${LIBGOPT_LIBRARIES}
    ${LIBCJSON_LIBRARIES}
    ${LIBYAML_LIBRARIES}
    ${READLINE_LIBRARIES}
    ${LUAJIT_LIB}
)

if (TARGET_OS_LINUX OR TARGET_OS_DEBIAN_FREEBSD)
    set (common_libraries ${common_libraries} dl)
endif()

if (TARGET_OS_FREEBSD AND NOT TARGET_OS_DEBIAN_FREEBSD)
    find_library (INTL intl)
    if (NOT INTL)
        message(FATAL_ERROR "intl library not found")
    else()
        set (common_libraries ${common_libraries} ${INTL})
    endif()
endif()

if (ENABLE_BACKTRACE AND HAVE_BFD)
    find_package (ZLIB)
    set (common_libraries ${common_libraries} bfd ${ZLIB_LIBRARIES})
    if (NOT TARGET_OS_DARWIN)
        set (common_libraries ${common_libraries} iberty)
    endif()
    if (TARGET_OS_FREEBSD AND NOT TARGET_OS_DEBIAN_FREEBSD)
        set (common_libraries ${common_libraries} iconv)
    endif()
endif()

set (common_libraries ${common_libraries} ${LIBUUID_LIBRARIES})
set (common_libraries ${common_libraries} PARENT_SCOPE)

add_subdirectory(lib)
add_subdirectory(box)
# Save CMAKE_XXX_FLAGS from this directory for config.h (used in --version)
set(TARANTOOL_C_FLAGS ${CMAKE_C_FLAGS} PARENT_SCOPE)
set(TARANTOOL_CXX_FLAGS ${CMAKE_CXX_FLAGS} PARENT_SCOPE)

add_executable(tarantool tarantool.cc)
add_dependencies(tarantool build_bundled_libs)
target_link_libraries(tarantool box ${common_libraries})

if (TARGET_OS_DARWIN)
    # Necessary to make LuaJIT work on Darwin, see
    # http://luajit.org/install.html
    set_target_properties(tarantool PROPERTIES
        LINK_FLAGS "-pagezero_size 10000 -image_base 100000000")
elseif (TARGET_OS_FREEBSD AND NOT TARGET_OS_DEBIAN_FREEBSD)
    # Helps symbol resolution of plug-ins
    set_target_properties(tarantool PROPERTIES LINK_FLAGS "-rdynamic")
endif()
install (TARGETS tarantool DESTINATION bin)
