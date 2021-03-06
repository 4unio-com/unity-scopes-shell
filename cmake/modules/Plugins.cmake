find_program(qmlplugindump_exe qmlplugindump)

if(NOT qmlplugindump_exe)
  msg(FATAL_ERROR "Could not locate qmlplugindump.")
endif()

# Creates target for copying and installing qmlfiles
#
# export_qmlfiles(plugin sub_path)
#
#
# Target to be created:
#   - plugin-qmlfiles - Copies the qml files (*.qml, *.js, qmldir) into the shadow build folder.

macro(export_qmlfiles PLUGIN PLUGIN_SUBPATH)

    file(GLOB QMLFILES
        *.qml
        *.js
        qmldir
    )

    # copy the qmldir file
    add_custom_target(${PLUGIN}-qmlfiles ALL
                        COMMAND cp ${QMLFILES} ${CMAKE_CURRENT_BINARY_DIR}
                        DEPENDS ${QMLFILES}
    )

    # install the qmlfiles file.
    install(FILES ${QMLFILES}
        DESTINATION ${SHELL_PLUGINDIR}/${PLUGIN_SUBPATH}
    )
endmacro(export_qmlfiles)


# Creates target for generating the qmltypes file for a plugin and installs plugin files
#
# export_qmlplugin(plugin version sub_path [TARGETS target1 [target2 ...]])
#
# TARGETS additional install targets (eg the plugin shared object)
#
# Target to be created:
#   - plugin-qmltypes - Generates the qmltypes file in the shadow build folder.

macro(export_qmlplugin PLUGIN VERSION PLUGIN_SUBPATH)
    set(multi_value_keywords TARGETS)
    cmake_parse_arguments(qmlplugin "" "" "${multi_value_keywords}" ${ARGN})

    # Only try to generate .qmltypes if not cross compiling
    if(NOT CMAKE_CROSSCOMPILING)
        # create the plugin.qmltypes file
        add_custom_target(${PLUGIN}-qmltypes ALL
            COMMAND UNITY_SCOPES_NO_LOCATION=1 UNITY_SCOPES_LIST_DELAY=10000 ${qmlplugindump_exe} -notrelocatable ${PLUGIN} ${VERSION} ${CMAKE_CURRENT_BINARY_DIR}/../ > ${CMAKE_CURRENT_BINARY_DIR}/plugin.qmltypes
        )
        add_dependencies(${PLUGIN}-qmltypes ${PLUGIN}-qmlfiles ${qmlplugin_TARGETS})

        # install the qmltypes file.
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/plugin.qmltypes
            DESTINATION ${SHELL_PLUGINDIR}/${PLUGIN_SUBPATH}
        )
    endif()

    # install the additional targets
    install(TARGETS ${qmlplugin_TARGETS}
        DESTINATION ${SHELL_PLUGINDIR}/${PLUGIN_SUBPATH}
    )
endmacro(export_qmlplugin)
