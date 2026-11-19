# The part of the toolkit that needs neither a display nor Skia, for
# services and daemons as well as applications.
set(CUTEFISH_TOOLKIT_BASE_PUBLIC_HEADERS
    application_bundle.h
    application_registry.h
    auto_reset.h
    byte_stream.h
    collator.h
    command_line.h
    dbus.h
    dbus_types.h
    elapsed_timer.h
    desktop_entry.h
    event_loop.h
    file.h
    file_watcher.h
    geometry.h
    hash.h
    i18n.h
    json.h
    local_socket.h
    locale.h
    logging.h
    lru_cache.h
    mime_apps.h
    mime_database.h
    paths.h
    process.h
    property_list.h
    region.h
    session_language.h
    settings.h
    signal.h
    strings.h
    thread_pool.h
    timer.h
    transform.h
    url.h
    uuid.h
    weak_ptr.h
)

add_library(cutefish-toolkit-base SHARED
    application_bundle.cpp
    application_registry.cpp
    collator.cpp
    command_line.cpp
    dbus.cpp
    desktop_entry.cpp
    event_loop.cpp
    file.cpp
    file_watcher.cpp
    geometry.cpp
    hash.cpp
    i18n.cpp
    json.cpp
    local_socket.cpp
    locale.cpp
    logging.cpp
    mime_apps.cpp
    mime_database.cpp
    mime_detection.cpp
    paths.cpp
    process.cpp
    property_list.cpp
    region.cpp
    session_language.cpp
    settings.cpp
    strings.cpp
    thread_pool.cpp
    timer.cpp
    transform.cpp
    url.cpp
    uuid.cpp
)

add_library(Cutefish::ToolkitBase ALIAS cutefish-toolkit-base)

set_target_properties(cutefish-toolkit-base PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME ToolkitBase
)

target_include_directories(cutefish-toolkit-base
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/cutefish-toolkit-base>
)

target_compile_definitions(cutefish-toolkit-base PRIVATE
    CUTEFISH_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
)

target_link_libraries(cutefish-toolkit-base
    PRIVATE
        PkgConfig::Icu
        PkgConfig::Systemd
        Threads::Threads
)

install(TARGETS cutefish-toolkit-base EXPORT CutefishToolkitTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES ${CUTEFISH_TOOLKIT_BASE_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cutefish-toolkit-base/base
)
