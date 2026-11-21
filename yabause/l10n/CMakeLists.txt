project(yabause-l10n)

set(LANGS ar de es fr it ja lt nl pt pt_BR ru sv zh_CN zh_TW)

if (UNIX AND NOT APPLE)
    foreach(LANG ${LANGS})
	    install(FILES "${CMAKE_CURRENT_LIST_DIR}/kronos_${LANG}.yts" DESTINATION "share/${YAB_PACKAGE}/yts" RENAME "${LANG}.yts")
    endforeach()
elseif (WIN32)
    foreach(LANG ${LANGS})
	    install(FILES "${CMAKE_CURRENT_LIST_DIR}/kronos_${LANG}.yts" DESTINATION "trans" RENAME "${LANG}.yts")
    endforeach()
endif ()
