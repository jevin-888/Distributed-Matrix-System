cmake_minimum_required(VERSION 3.21)

project(DistributedMatrixSystem
    VERSION 1.0.0
    DESCRIPTION "Distributed screen matrix controller for RK3566 Linux nodes"
    LANGUAGES CXX
)

add_compile_definitions(DMS_VERSION="${PROJECT_VERSION}")

include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.11 QUIET)

function(dms_configure_target target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    if(ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
endfunction()

add_library(common STATIC
    src/common/DiscoveryProtocol.cpp
    src/common/Logger.cpp
    src/common/Protocol.cpp
    src/common/SyncTimestampGenerator.cpp
    src/common/VideoCacheManager.cpp
)
target_include_directories(common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/distributed_matrix>
)
if(nlohmann_json_FOUND)
    target_link_libraries(common PUBLIC nlohmann_json::nlohmann_json)
else()
    message(STATUS "nlohmann_json not found; using bundled header")
    target_include_directories(common PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include>
    )
endif()
target_link_libraries(common PUBLIC Threads::Threads CURL::libcurl)
dms_configure_target(common)

add_library(kvm STATIC
    src/kvm/KvmAgent.cpp
    src/kvm/KvmSessionManager.cpp
)
target_link_libraries(kvm PUBLIC common)
dms_configure_target(kvm)

add_library(master STATIC
    src/master/CommandBroadcaster.cpp
    src/master/HTTPFileServer.cpp
    src/master/LayoutCalculator.cpp
    src/master/MasterNodeApp.cpp
    src/master/NodeDiscovery.cpp
)
target_link_libraries(master PUBLIC common kvm)
dms_configure_target(master)

add_library(slave STATIC
    src/node/CommandReceiver.cpp
    src/node/MediaPlayer.cpp
    src/node/SlaveNodeApp.cpp
    src/oled/OledDisplay.cpp
)
target_link_libraries(slave PUBLIC common kvm ${CMAKE_DL_LIBS})
dms_configure_target(slave)

add_executable(distributed-matrix src/main.cpp)
target_link_libraries(distributed-matrix PRIVATE master slave)
dms_configure_target(distributed-matrix)

if(BUILD_TESTING)
    find_program(BASH_EXECUTABLE bash REQUIRED)

    add_executable(oled_tests tests/oled_tests.cpp src/oled/OledDisplay.cpp src/common/Logger.cpp)
    target_include_directories(oled_tests PRIVATE include)
    target_link_libraries(oled_tests PRIVATE Threads::Threads)
    target_link_options(oled_tests PRIVATE
        -Wl,--wrap=open,--wrap=close,--wrap=access,--wrap=write,--wrap=ioctl,--wrap=usleep)
    dms_configure_target(oled_tests)
    add_test(NAME oled_tests COMMAND oled_tests)

    add_executable(oled_boot_guard_tests tests/oled_boot_guard_tests.cpp)
    dms_configure_target(oled_boot_guard_tests)
    add_test(NAME oled_boot_guard_tests COMMAND oled_boot_guard_tests)

    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE master)
    dms_configure_target(core_tests)
    add_test(NAME core_tests COMMAND core_tests)

    add_executable(cache_tests tests/cache_tests.cpp)
    target_link_libraries(cache_tests PRIVATE common)
    dms_configure_target(cache_tests)
    add_test(NAME cache_tests COMMAND cache_tests)

    add_executable(component_tests tests/component_tests.cpp)
    target_link_libraries(component_tests PRIVATE master slave)
    dms_configure_target(component_tests)
    add_test(NAME component_tests COMMAND component_tests)
    set_tests_properties(component_tests PROPERTIES TIMEOUT 35)

    add_test(
        NAME http_integration
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/http_integration.sh
                $<TARGET_FILE:distributed-matrix> ${CMAKE_CURRENT_SOURCE_DIR}/web
    )
    add_test(
        NAME config_integration
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_integration.sh
                $<TARGET_FILE:distributed-matrix> ${CMAKE_CURRENT_SOURCE_DIR}/web
    )
    add_test(
        NAME package_policy
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_policy.sh
                ${CMAKE_CURRENT_SOURCE_DIR}/system/firefly-rk356x/package/distributed-matrix
    )
    add_test(
        NAME hardware_window_policy
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/hardware_window_policy.sh
                ${CMAKE_CURRENT_SOURCE_DIR}/src/node/MediaPlayer.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/include/node/MediaPlayer.h
    )
    set_tests_properties(http_integration config_integration package_policy hardware_window_policy
                         PROPERTIES TIMEOUT 20)
endif()

install(TARGETS distributed-matrix common kvm master slave
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/distributed_matrix
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY web/ DESTINATION ${CMAKE_INSTALL_DATADIR}/distributed_matrix/web)
install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/distributed_matrix/assets)
install(DIRECTORY config/ DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/distributed_matrix)
install(PROGRAMS
    scripts/health-check.sh
    scripts/collect-diagnostic-info.sh
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)

message(STATUS "Distributed Matrix System ${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Build tests: ${BUILD_TESTING}")
message(STATUS "Enable ASAN: ${ENABLE_ASAN}")
