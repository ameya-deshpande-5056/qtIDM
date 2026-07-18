cmake_minimum_required(VERSION 3.24)

project(qtIDM VERSION 0.1.1 LANGUAGES CXX)

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
set(QTIDM_BROWSER_PACKAGE_DIR "${CMAKE_SOURCE_DIR}/browser/packages" CACHE PATH "Directory containing signed qtidm-chrome.crx and qtidm-firefox.xpi packages")
option(QTIDM_ENABLE_NATIVE_TUNING "Build for the current CPU with -march=native" OFF)
option(QTIDM_INSTALL_NATIVE_MESSAGING_HOSTS "Install browser native messaging host manifests" ON)
option(QTIDM_INSTALL_EXTERNAL_CHROME_EXTENSION "Register a bundled CRX as an external Google Chrome extension" OFF)
option(QTIDM_BUILD_TESTS "Build qtIDM test executables" ON)

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

set(QTIDM_CHROME_EXTENSION_PACKAGE "${QTIDM_BROWSER_PACKAGE_DIR}/qtidm-chrome.crx")
set(QTIDM_FIREFOX_EXTENSION_PACKAGE "${QTIDM_BROWSER_PACKAGE_DIR}/qtidm-firefox.xpi")
set(QTIDM_HAVE_BROWSER_PACKAGES OFF)
if(EXISTS "${QTIDM_CHROME_EXTENSION_PACKAGE}" OR EXISTS "${QTIDM_FIREFOX_EXTENSION_PACKAGE}")
    if(NOT EXISTS "${QTIDM_CHROME_EXTENSION_PACKAGE}" OR NOT EXISTS "${QTIDM_FIREFOX_EXTENSION_PACKAGE}")
        message(FATAL_ERROR "QTIDM_BROWSER_PACKAGE_DIR must contain both qtidm-chrome.crx and qtidm-firefox.xpi.")
    endif()
    set(QTIDM_HAVE_BROWSER_PACKAGES ON)
endif()

if(QTIDM_INSTALL_EXTERNAL_CHROME_EXTENSION)
    if(NOT QTIDM_HAVE_BROWSER_PACKAGES)
        message(FATAL_ERROR "External Chrome registration requires signed browser packages in QTIDM_BROWSER_PACKAGE_DIR.")
    endif()
    file(READ "${CMAKE_SOURCE_DIR}/browser/chrome/manifest.json" QTIDM_CHROME_MANIFEST_JSON)
    string(JSON QTIDM_BROWSER_EXTENSION_VERSION GET "${QTIDM_CHROME_MANIFEST_JSON}" version)
    configure_file(
        browser/chrome/external-extension.json.in
        "${CMAKE_BINARY_DIR}/generated/browser/chrome/${QTIDM_CHROME_EXTENSION_ID}.json"
        @ONLY
    )
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -flto)
    add_link_options(-flto)
endif()

if(QTIDM_ENABLE_NATIVE_TUNING)
    add_compile_options(-march=native)
endif()

set(QTIDM_SOURCES
    src/app/Logger.cpp
    src/app/Paths.cpp
    src/app/SingleInstance.cpp
    src/core/DownloadTypes.cpp
    src/core/CurlEpollDownloader.cpp
    src/core/DownloadScheduler.cpp
    src/core/ArchiveExtractor.cpp
    src/core/SiteGrabber.cpp
    src/core/MediaDownloader.cpp
    src/core/SparseFileWriter.cpp
    src/core/ZipPreview.cpp
    src/integration/ImportExportService.cpp
    src/integration/CredentialVault.cpp
    src/integration/MeteredNetworkMonitor.cpp
    src/integration/SocialMediaExtractor.cpp
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
    src/app/Logger.cpp
    src/app/Paths.cpp
)

target_include_directories(qtIDM-native-host PRIVATE src)
target_link_libraries(qtIDM-native-host PRIVATE Qt6::Core Qt6::DBus)

if(QTIDM_BUILD_TESTS)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

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

add_test(NAME qtIDM_chrome_extension_e2e_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/browser_extension_e2e.py --browser chrome
)
set_tests_properties(qtIDM_chrome_extension_e2e_tests PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 60
)

add_test(NAME qtIDM_firefox_extension_e2e_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/browser_extension_e2e.py --browser firefox
)
set_tests_properties(qtIDM_firefox_extension_e2e_tests PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 60
)

add_test(NAME qtIDM_browser_packaging_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/browser_packaging_test.py
)
set_tests_properties(qtIDM_browser_packaging_tests PROPERTIES TIMEOUT 60)

find_program(QTIDM_NODE_EXECUTABLE NAMES node nodejs)
if(QTIDM_NODE_EXECUTABLE)
    add_test(NAME qtIDM_browser_interception_tests
        COMMAND ${QTIDM_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/browser_interception_test.cjs
    )
endif()

add_executable(qtIDM_scheduler_tests
    tests/DownloadSchedulerTest.cpp
    src/app/Paths.cpp
    src/core/CurlEpollDownloader.cpp
    src/core/DownloadScheduler.cpp
    src/core/ArchiveExtractor.cpp
    src/core/DownloadTypes.cpp
    src/core/SparseFileWriter.cpp
    src/integration/CredentialVault.cpp
)

target_include_directories(qtIDM_scheduler_tests PRIVATE src)
target_link_libraries(qtIDM_scheduler_tests PRIVATE Qt6::Core Qt6::Test CURL::libcurl Threads::Threads)
add_test(NAME qtIDM_scheduler_tests COMMAND qtIDM_scheduler_tests)

add_executable(qtIDM_archive_extractor_tests
    tests/ArchiveExtractorTest.cpp
    src/core/ArchiveExtractor.cpp
)
target_include_directories(qtIDM_archive_extractor_tests PRIVATE src)
target_link_libraries(qtIDM_archive_extractor_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME qtIDM_archive_extractor_tests COMMAND qtIDM_archive_extractor_tests)

add_executable(qtIDM_credential_vault_tests
    tests/CredentialVaultTest.cpp
    src/integration/CredentialVault.cpp
)
target_include_directories(qtIDM_credential_vault_tests PRIVATE src)
target_link_libraries(qtIDM_credential_vault_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME qtIDM_credential_vault_tests COMMAND qtIDM_credential_vault_tests)

add_executable(qtIDM_metered_network_tests
    tests/MeteredNetworkMonitorTest.cpp
    src/integration/MeteredNetworkMonitor.cpp
)
target_include_directories(qtIDM_metered_network_tests PRIVATE src)
target_link_libraries(qtIDM_metered_network_tests PRIVATE Qt6::Core Qt6::DBus Qt6::Test)
add_test(NAME qtIDM_metered_network_tests COMMAND qtIDM_metered_network_tests)

add_executable(qtIDM_social_media_extractor_tests
    tests/SocialMediaExtractorTest.cpp
    src/integration/SocialMediaExtractor.cpp
)
target_include_directories(qtIDM_social_media_extractor_tests PRIVATE src)
target_link_libraries(qtIDM_social_media_extractor_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME qtIDM_social_media_extractor_tests COMMAND qtIDM_social_media_extractor_tests)

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

add_executable(qtIDM_media_downloader_tests
    tests/MediaDownloaderTest.cpp
    src/app/Logger.cpp
    src/app/Paths.cpp
    src/core/DownloadTypes.cpp
    src/core/MediaDownloader.cpp
)

target_include_directories(qtIDM_media_downloader_tests PRIVATE src)
target_link_libraries(qtIDM_media_downloader_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME qtIDM_media_downloader_tests COMMAND qtIDM_media_downloader_tests)

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
endif()

install(TARGETS qtIDM qtIDM-native-host RUNTIME DESTINATION bin)
install(FILES assets/io.github.qtidm.qtidm.desktop DESTINATION share/applications)
install(FILES assets/io.github.qtidm.qtidm.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES assets/io.github.qtidm.qtidm.appdata.xml DESTINATION share/metainfo)
install(FILES assets/io.github.qtidm.qtidm.xml DESTINATION share/mime/packages)
if(QTIDM_INSTALL_NATIVE_MESSAGING_HOSTS)
    install(
        FILES ${CMAKE_BINARY_DIR}/generated/browser/native/io.github.qtidm.native.firefox.json
        DESTINATION lib/mozilla/native-messaging-hosts
        RENAME io.github.qtidm.native.json
    )
    install(
        FILES ${CMAKE_BINARY_DIR}/generated/browser/native/io.github.qtidm.native.chrome.json
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/opt/chrome/native-messaging-hosts
        RENAME io.github.qtidm.native.json
    )
    install(
        FILES ${CMAKE_BINARY_DIR}/generated/browser/native/io.github.qtidm.native.chrome.json
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/chromium/native-messaging-hosts
        RENAME io.github.qtidm.native.json
    )
endif()
install(
    DIRECTORY browser/chrome
    DESTINATION share/qtidm/browser
    PATTERN "external-extension.json.in" EXCLUDE
)
install(DIRECTORY browser/firefox DESTINATION share/qtidm/browser)
if(QTIDM_HAVE_BROWSER_PACKAGES)
    install(
        FILES
            "${QTIDM_CHROME_EXTENSION_PACKAGE}"
            "${QTIDM_FIREFOX_EXTENSION_PACKAGE}"
        DESTINATION share/qtidm/browser/packages
    )
endif()
if(QTIDM_INSTALL_EXTERNAL_CHROME_EXTENSION)
    install(
        FILES "${CMAKE_BINARY_DIR}/generated/browser/chrome/${QTIDM_CHROME_EXTENSION_ID}.json"
        DESTINATION /opt/google/chrome/extensions
    )
endif()
