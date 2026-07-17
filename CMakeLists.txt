cmake_minimum_required(VERSION 3.24)

project(qtIDM VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "qtIDM is Linux-only.")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets DBus Test)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
include(GNUInstallDirs)

set(QTIDM_CHROME_EXTENSION_ID "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" CACHE STRING "Chrome extension ID allowed to use native messaging")
set(QTIDM_FIREFOX_EXTENSION_ID "qtidm@io.github.qtidm" CACHE STRING "Firefox extension ID allowed to use native messaging")
option(QTIDM_ENABLE_NATIVE_TUNING "Build for the current CPU with -march=native" OFF)
option(QTIDM_INSTALL_NATIVE_MESSAGING_HOSTS "Install browser native messaging host manifests" ON)

string(LENGTH "${QTIDM_CHROME_EXTENSION_ID}" QTIDM_CHROME_EXTENSION_ID_LENGTH)
if(NOT QTIDM_CHROME_EXTENSION_ID_LENGTH EQUAL 32 OR NOT QTIDM_CHROME_EXTENSION_ID MATCHES "^[a-p]+$")
    message(FATAL_ERROR "QTIDM_CHROME_EXTENSION_ID must be a 32-character Chrome extension ID using letters a-p.")
endif()

configure_file(
    browser/native/io.github.qtidm.native.chrome.json.in
    ${CMAKE_BINARY_DIR}/generated/browser/native/io.github.qtidm.native.chrome.json
    @ONLY
)

configure_file(
    browser/native/io.github.qtidm.native.firefox.json.in
    ${CMAKE_BINARY_DIR}/generated/browser/native/io.github.qtidm.native.firefox.json
    @ONLY
)

add_compile_options(-Wall -Wextra -Wpedantic)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -flto)
    add_link_options(-flto)
endif()

if(QTIDM_ENABLE_NATIVE_TUNING)
    add_compile_options(-march=native)
endif()

set(QTIDM_SOURCES
    src/app/Paths.cpp
    src/app/SingleInstance.cpp
    src/core/DownloadTypes.cpp
    src/core/CurlEpollDownloader.cpp
    src/core/DownloadScheduler.cpp
    src/core/SiteGrabber.cpp
    src/core/SparseFileWriter.cpp
    src/core/ZipPreview.cpp
    src/integration/ImportExportService.cpp
    src/storage/DownloadRepository.cpp
    src/theme/ThemeManager.cpp
    src/ui/MainWindow.cpp
    src/ui/SegmentGrid.cpp
)

add_executable(qtIDM
    src/main.cpp
    ${QTIDM_SOURCES}
)

target_include_directories(qtIDM PRIVATE src)
target_compile_definitions(qtIDM PRIVATE QTIDM_VERSION="${PROJECT_VERSION}")
target_link_libraries(qtIDM PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::DBus
    CURL::libcurl
    SQLite::SQLite3
    Threads::Threads
)

add_executable(qtIDM-native-host
    src/integration/NativeMessagingHost.cpp
)

target_link_libraries(qtIDM-native-host PRIVATE Qt6::Core Qt6::DBus)

enable_testing()

add_executable(qtIDM_core_tests
    tests/DownloadRepositoryTest.cpp
    src/core/DownloadTypes.cpp
    src/app/Paths.cpp
    src/storage/DownloadRepository.cpp
)

target_include_directories(qtIDM_core_tests PRIVATE src)
target_link_libraries(qtIDM_core_tests PRIVATE Qt6::Core Qt6::Test SQLite::SQLite3)
add_test(NAME qtIDM_core_tests COMMAND qtIDM_core_tests)

add_executable(qtIDM_download_types_tests
    tests/DownloadTypesTest.cpp
    src/core/DownloadTypes.cpp
)

target_include_directories(qtIDM_download_types_tests PRIVATE src)
target_link_libraries(qtIDM_download_types_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME qtIDM_download_types_tests COMMAND qtIDM_download_types_tests)

add_executable(qtIDM_import_export_tests
    tests/ImportExportServiceTest.cpp
    src/core/DownloadTypes.cpp
    src/integration/ImportExportService.cpp
    src/storage/DownloadRepository.cpp
)

target_include_directories(qtIDM_import_export_tests PRIVATE src)
target_link_libraries(qtIDM_import_export_tests PRIVATE Qt6::Core Qt6::Test SQLite::SQLite3)
add_test(NAME qtIDM_import_export_tests COMMAND qtIDM_import_export_tests)

add_executable(qtIDM_sparse_file_writer_tests
    tests/SparseFileWriterTest.cpp
    src/core/SparseFileWriter.cpp
)

target_include_directories(qtIDM_sparse_file_writer_tests PRIVATE src)
target_link_libraries(qtIDM_sparse_file_writer_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME qtIDM_sparse_file_writer_tests COMMAND qtIDM_sparse_file_writer_tests)

add_executable(qtIDM_site_grabber_tests
    tests/SiteGrabberTest.cpp
    src/core/SiteGrabber.cpp
)

target_include_directories(qtIDM_site_grabber_tests PRIVATE src)
target_link_libraries(qtIDM_site_grabber_tests PRIVATE Qt6::Core Qt6::Test CURL::libcurl)
add_test(NAME qtIDM_site_grabber_tests COMMAND qtIDM_site_grabber_tests)

add_executable(qtIDM_scheduler_tests
    tests/DownloadSchedulerTest.cpp
    src/app/Paths.cpp
    src/core/CurlEpollDownloader.cpp
    src/core/DownloadScheduler.cpp
    src/core/DownloadTypes.cpp
    src/core/SparseFileWriter.cpp
)

target_include_directories(qtIDM_scheduler_tests PRIVATE src)
target_link_libraries(qtIDM_scheduler_tests PRIVATE Qt6::Core Qt6::Test CURL::libcurl Threads::Threads)
add_test(NAME qtIDM_scheduler_tests COMMAND qtIDM_scheduler_tests)

add_executable(qtIDM_downloader_integration_tests
    tests/DownloaderIntegrationTest.cpp
    src/core/CurlEpollDownloader.cpp
    src/core/DownloadTypes.cpp
    src/core/SparseFileWriter.cpp
)

target_include_directories(qtIDM_downloader_integration_tests PRIVATE src)
target_compile_definitions(qtIDM_downloader_integration_tests PRIVATE QTIDM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(qtIDM_downloader_integration_tests PRIVATE Qt6::Core Qt6::Test CURL::libcurl Threads::Threads)
add_test(NAME qtIDM_downloader_integration_tests COMMAND qtIDM_downloader_integration_tests)

add_executable(qtIDM_performance_smoke_tests
    tests/PerformanceSmokeTest.cpp
    src/core/DownloadTypes.cpp
    src/storage/DownloadRepository.cpp
)

target_include_directories(qtIDM_performance_smoke_tests PRIVATE src)
target_link_libraries(qtIDM_performance_smoke_tests PRIVATE Qt6::Core Qt6::Test SQLite::SQLite3)
add_test(NAME qtIDM_performance_smoke_tests COMMAND qtIDM_performance_smoke_tests)

add_executable(qtIDM_packaging_metadata_tests
    tests/PackagingMetadataTest.cpp
)

target_compile_definitions(qtIDM_packaging_metadata_tests PRIVATE QTIDM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_definitions(qtIDM_packaging_metadata_tests PRIVATE QTIDM_BINARY_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(qtIDM_packaging_metadata_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME qtIDM_packaging_metadata_tests COMMAND qtIDM_packaging_metadata_tests)

add_executable(qtIDM_zip_preview_tests
    tests/ZipPreviewTest.cpp
    src/core/ZipPreview.cpp
)

target_include_directories(qtIDM_zip_preview_tests PRIVATE src)
target_link_libraries(qtIDM_zip_preview_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME qtIDM_zip_preview_tests COMMAND qtIDM_zip_preview_tests)

install(TARGETS qtIDM qtIDM-native-host RUNTIME DESTINATION bin)
install(FILES assets/io.github.qtidm.qtidm.desktop DESTINATION share/applications)
install(FILES assets/io.github.qtidm.qtidm.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES assets/io.github.qtidm.qtidm.metainfo.xml DESTINATION share/metainfo)
install(FILES assets/io.github.qtidm.qtidm.xml DESTINATION share/mime/packages)
if(QTIDM_INSTALL_NATIVE_MESSAGING_HOSTS)
    install(FILES ${CMAKE_BINARY_DIR}/generated/browser/native/io.github.qtidm.native.firefox.json DESTINATION ${CMAKE_INSTALL_LIBDIR}/mozilla/native-messaging-hosts)
    install(FILES ${CMAKE_BINARY_DIR}/generated/browser/native/io.github.qtidm.native.chrome.json DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/opt/chrome/native-messaging-hosts)
endif()
install(DIRECTORY browser/chrome DESTINATION share/qtidm/browser)
install(DIRECTORY browser/firefox DESTINATION share/qtidm/browser)
