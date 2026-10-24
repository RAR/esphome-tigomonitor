cmake_minimum_required(VERSION 3.20)
project(tigomonitord CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(TIGOD_VERSION "dev" CACHE STRING "Version string reported by tigomonitord")

find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)
find_package(nlohmann_json 3 REQUIRED)
find_package(Threads REQUIRED)

add_executable(tigomonitord
  src/main.cpp
  src/cca_ble.cpp
  src/cca_http.cpp
  src/cloud.cpp
  src/fsutil.cpp
  src/history.cpp
  src/http_server.cpp
  src/monitor.cpp
  src/mqtt_publisher.cpp
  src/serial_port.cpp
  src/settings.cpp
  src/topology.cpp
)
# The decoder and node-identity rules are shared with the ESP32 component.
target_include_directories(tigomonitord PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../components/tigo_monitor)
target_compile_definitions(tigomonitord PRIVATE TIGOD_VERSION="${TIGOD_VERSION}")
target_compile_options(tigomonitord PRIVATE -Wall -Wextra)
target_link_libraries(tigomonitord PRIVATE PkgConfig::MOSQUITTO PkgConfig::HTTPLIB PkgConfig::SQLITE PkgConfig::SYSTEMD PkgConfig::CRYPTO nlohmann_json::nlohmann_json Threads::Threads)

install(TARGETS tigomonitord RUNTIME DESTINATION bin)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../components/tigo_server/web/app.html
              # The 16px cut of the brand mark, the one the firmware inlines.
              ${CMAKE_CURRENT_SOURCE_DIR}/../site/public/favicon.svg
        DESTINATION share/tigomonitor/web)
