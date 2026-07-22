# Include from a Zephyr application CMakeLists.txt after find_package(Zephyr).
# The application must provide OpenAMP and libmetal at the standard module paths.

set(WITH_ZEPHYR ON CACHE BOOL "" FORCE)
set(WITH_ZEPHYR_LIB ON CACHE BOOL "" FORCE)
set(WITH_LIBMETAL_FIND OFF CACHE BOOL "" FORCE)
set(WITH_DOC OFF CACHE BOOL "" FORCE)
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_PROXY OFF CACHE BOOL "" FORCE)
set(WITH_DCACHE ON CACHE BOOL "" FORCE)
set(OFFSETS_H_TARGET offsets_h)

add_subdirectory(${ZEPHYR_BASE}/modules/hal/libmetal
	${CMAKE_CURRENT_BINARY_DIR}/libmetal)
add_subdirectory(${ZEPHYR_BASE}/modules/lib/open-amp
	${CMAKE_CURRENT_BINARY_DIR}/open-amp)

target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/beau_rpmsg.c)
target_include_directories(app PRIVATE
	${ZEPHYR_BASE}/modules/lib/open-amp/lib/include)
target_link_libraries(open_amp PRIVATE metal)
target_link_libraries(app PRIVATE open_amp metal)
