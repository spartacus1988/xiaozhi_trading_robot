if(${IDF_TARGET} STREQUAL "linux")
idf_component_register(SRCS "touch_sensor_lowlevel_simulation.c" "simulation_data.c"
                        INCLUDE_DIRS "include")
message(STATUS "Building touch sensor lowlevel simulator")
else()
set(requires "driver")
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
list(APPEND requires "esp_driver_touch_sens" "esp_driver_gpio")
endif()
idf_component_register(SRCS "touch_sensor_lowlevel.c"
                        INCLUDE_DIRS "include"
                        REQUIRES "${requires}")
message(STATUS "Building touch sensor lowlevel")
endif()

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
message(STATUS "Using local touch sensor lowlevel library")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-ignored-qualifiers -Wno-int-to-pointer-cast -Wno-discarded-array-qualifiers)
