cmake_minimum_required(VERSION 3.16)
project(bgmon VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)   # the SDK structs end in flexible array members inside packed structs

option(BGMON_BUILD_GUI   "Build the Qt GUI" ON)
option(BGMON_BUILD_TESTS "Build the core tests (no Qt needed)" ON)
option(BGMON_REGEN_TABLES "Regenerate src/core/bgapi_tables.c from third_party/silabs with Python" OFF)

set(SILABS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/silabs)

if(BGMON_REGEN_TABLES)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/core/bgapi_tables.c
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_bgapi_tables.py ${SILABS_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core/bgapi_tables.c
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_bgapi_tables.py ${SILABS_DIR}/sl_bt_api.h
            ${SILABS_DIR}/sli_bt_api.h ${SILABS_DIR}/sl_status.h
    COMMENT "Generating BGAPI tables from the SDK headers")
endif()

# ---- protocol core (no Qt) -----------------------------------------------
add_library(bgmon_core STATIC src/core/bgapi_core.cpp src/core/bgapi_tables.c)
target_include_directories(bgmon_core PUBLIC src/core PRIVATE ${SILABS_DIR})
if(MSVC)
  target_compile_definitions(bgmon_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- GUI -----------------------------------------------------------------
if(BGMON_BUILD_GUI)
  find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets SerialPort)
  find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets SerialPort)
  set(CMAKE_AUTOMOC ON)
  add_executable(bgmon WIN32
    src/gui/main.cpp
    src/gui/mainwindow.cpp src/gui/mainwindow.h
    src/gui/framemodel.cpp src/gui/framemodel.h)
  target_link_libraries(bgmon PRIVATE bgmon_core Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::SerialPort)
  if(WIN32)
    # Reuse the TMS Configurator's registry port scan so com0com ports
    # (CNCA0/CNCB0 or renamed) are listed even when Qt does not enumerate them.
    target_sources(bgmon PRIVATE third_party/tms_com/serial_transport.c)
    target_include_directories(bgmon PRIVATE third_party/tms_com ${SILABS_DIR})
    target_compile_definitions(bgmon PRIVATE BGMON_HAVE_TMS_SCAN _CRT_SECURE_NO_WARNINGS)
  endif()
endif()

# ---- tests ---------------------------------------------------------------
if(BGMON_BUILD_TESTS)
  enable_testing()
  add_executable(test_core tests/test_core.cpp)
  target_include_directories(test_core PRIVATE ${SILABS_DIR})
  target_link_libraries(test_core PRIVATE bgmon_core)
  add_test(NAME core COMMAND test_core)

  # The real Silicon Labs host library (the Configurator's copies) as the host app.
  add_executable(test_proxy_bglib tests/test_proxy_bglib.cpp
    tests/silabs_host/sl_bt_ncp_host.c tests/silabs_host/sl_bt_ncp_host_api.c
    tests/silabs_host/bt_transport_adapter.c)
  target_include_directories(test_proxy_bglib PRIVATE ${SILABS_DIR} third_party/tms_com tests/silabs_host)
  target_link_libraries(test_proxy_bglib PRIVATE bgmon_core)
  add_test(NAME proxy_bglib COMMAND test_proxy_bglib)

  if(MINGW)
    # Self-contained test programs: they run on any Windows PC without DLLs.
    target_link_options(test_core PRIVATE -static)
    target_link_options(test_proxy_bglib PRIVATE -static)
  endif()
endif()
