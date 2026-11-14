# cutefish_add_translations(<domain>
#     SOURCES <files or directories>...
#     [DESKTOP_FILES <files>...])
#
# Builds and installs po/<language>.po of the calling directory as the
# gettext catalog <domain>, and writes each desktop file, translated from the
# same catalogs, into the current binary directory for the caller to install.
# Without msgfmt the program builds untranslated.
#
# update-translations extracts the strings of SOURCES (C++ files, directories
# searched for them) and DESKTOP_FILES into po/<domain>.pot and merges it
# into every po/<language>.po. check-translations, and the translations test,
# check every catalog with msgfmt, the "{name}" placeholders included.

function(cutefish_add_translations domain)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DESKTOP_FILES")
    set(po_directory ${CMAKE_CURRENT_SOURCE_DIR}/po)
    file(GLOB po_files CONFIGURE_DEPENDS ${po_directory}/*.po)
    list(SORT po_files)
    find_program(CUTEFISH_MSGFMT msgfmt)
    find_program(CUTEFISH_XGETTEXT xgettext)
    find_program(CUTEFISH_MSGMERGE msgmerge)

    if(CUTEFISH_XGETTEXT AND CUTEFISH_MSGMERGE)
        # A list would be split into separate arguments on the way to the script.
        string(REPLACE ";" "|" sources "${arg_SOURCES}")
        string(REPLACE ";" "|" desktop_files "${arg_DESKTOP_FILES}")
        add_custom_target(update-translations
            COMMAND ${CMAKE_COMMAND} -DDOMAIN=${domain} -DSOURCE_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}
                    -DSOURCES=${sources} -DDESKTOP_FILES=${desktop_files}
                    -DXGETTEXT=${CUTEFISH_XGETTEXT} -DMSGMERGE=${CUTEFISH_MSGMERGE}
                    -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CutefishTranslationsUpdate.cmake
            VERBATIM)
    endif()

    if(NOT CUTEFISH_MSGFMT)
        message(STATUS "msgfmt not found: ${domain} is built without translations")
        foreach(desktop_file ${arg_DESKTOP_FILES})
            get_filename_component(name ${desktop_file} NAME)
            configure_file(${desktop_file} ${CMAKE_CURRENT_BINARY_DIR}/${name} COPYONLY)
        endforeach()
        return()
    endif()

    set(catalogs "")
    set(languages "")
    foreach(po_file ${po_files})
        get_filename_component(language ${po_file} NAME_WE)
        list(APPEND languages ${language})
        set(catalog ${CMAKE_CURRENT_BINARY_DIR}/locale/${language}/${domain}.mo)
        add_custom_command(OUTPUT ${catalog}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/locale/${language}
            COMMAND ${CUTEFISH_MSGFMT} -o ${catalog} ${po_file}
            DEPENDS ${po_file}
            VERBATIM)
        list(APPEND catalogs ${catalog})
        install(FILES ${catalog} DESTINATION ${CMAKE_INSTALL_LOCALEDIR}/${language}/LC_MESSAGES)
    endforeach()

    # msgfmt --desktop reads the languages from a LINGUAS file beside the
    # catalogs.
    set(desktop_po_directory ${CMAKE_CURRENT_BINARY_DIR}/desktop-po)
    string(REPLACE ";" "\n" linguas "${languages}")
    file(WRITE ${desktop_po_directory}/LINGUAS "${linguas}\n")
    # Copied once here: a copy in each desktop file's rule races with the
    # others under a parallel build.
    foreach(po_file ${po_files})
        configure_file(${po_file} ${desktop_po_directory}/ COPYONLY)
    endforeach()
    set(desktop_outputs "")
    foreach(desktop_file ${arg_DESKTOP_FILES})
        get_filename_component(name ${desktop_file} NAME)
        get_filename_component(template ${desktop_file} ABSOLUTE)
        set(output ${CMAKE_CURRENT_BINARY_DIR}/${name})
        add_custom_command(OUTPUT ${output}
            COMMAND ${CUTEFISH_MSGFMT} --desktop --template=${template} -d ${desktop_po_directory} -o ${output}
            DEPENDS ${template} ${po_files}
            VERBATIM)
        list(APPEND desktop_outputs ${output})
    endforeach()

    add_custom_target(${domain}-translations ALL DEPENDS ${catalogs} ${desktop_outputs})

    string(REPLACE ";" "|" po_list "${po_files}")
    add_custom_target(check-translations
        COMMAND ${CMAKE_COMMAND} -DPO_FILES=${po_list} -DMSGFMT=${CUTEFISH_MSGFMT}
                -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CutefishTranslationsCheck.cmake
        VERBATIM)
    if(BUILD_TESTING AND po_files)
        add_test(NAME translations
            COMMAND ${CMAKE_COMMAND} -DPO_FILES=${po_list} -DMSGFMT=${CUTEFISH_MSGFMT}
                    -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CutefishTranslationsCheck.cmake)
    endif()
endfunction()
