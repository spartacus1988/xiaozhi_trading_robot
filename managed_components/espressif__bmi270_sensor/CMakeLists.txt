if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bmi270_api.c")
file(GLOB sources "*.c")
set(priv_include_dirs "priv_include")
else()
set(sources "")
set(priv_include_dirs)
endif()

idf_component_register( SRCS ${sources}
                        INCLUDE_DIRS "include"
                        PRIV_INCLUDE_DIRS ${priv_include_dirs}
                        REQUIRES i2c_bus
                        PRIV_REQUIRES esp_timer)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bmi270_api.c")
    set(bmi270_lib_path "${CMAKE_CURRENT_SOURCE_DIR}/${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}/${CONFIG_IDF_TARGET}/libbmi270_sensor.a")

    if(CONFIG_IDF_TARGET STREQUAL "esp32p4")
        # this component doesn't support idf < 5.3
        if(IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR EQUAL 3)
            # for idf v5.3, only rev1 is supported
            set(bmi270_esp32p4_rev "rev1")
        elseif(CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
            set(bmi270_esp32p4_rev "rev1")
        else()
            set(bmi270_esp32p4_rev "rev3")
        endif()

        set(bmi270_esp32p4_lib_path "${CMAKE_CURRENT_SOURCE_DIR}/${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}/${CONFIG_IDF_TARGET}/${bmi270_esp32p4_rev}/libbmi270_sensor.a")
        if(NOT EXISTS "${bmi270_esp32p4_lib_path}")
            message(FATAL_ERROR "Missing BMI270 prebuilt library: ${bmi270_esp32p4_lib_path}")
        endif()
        set(bmi270_lib_path "${bmi270_esp32p4_lib_path}")
    endif()

    add_prebuilt_library(bmi270_lib "${bmi270_lib_path}")
    target_include_directories(${COMPONENT_LIB} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(${COMPONENT_LIB} INTERFACE bmi270_lib)
else()
    include(package_manager)
    cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wignored-qualifiers)
endif()
