project( kronos-qt )

yab_port_start()

option(YAB_USE_QT5 "Use Qt 5 if available." ON)

option(YAB_USE_QT5_SPECIFIC "Use dedicated QT setup" OFF)

if(YAB_USE_QT5)
  find_package(Qt5 COMPONENTS Widgets QUIET)
  find_package(Qt5 COMPONENTS OpenGL REQUIRED QUIET)
  find_package(Qt5 COMPONENTS Multimedia REQUIRED QUIET)
  add_definitions(-DHAVE_QT_MULTIMEDIA=1)
endif()

if(Qt5_FOUND)
    message(STATUS "Qt5 Found")

else()
	message(STATUS "NO Qt5_FOUND")
	return ()
endif()

# Webcam for the ST-V Print Club games (ctrl/src/pclub.c), through
# openpnp-capture (MIT licence): DirectShow on Windows, V4L2 on Linux,
# AVFoundation on macOS. Its sources are fetched at a fixed commit and only
# the library is built here, as a static library: its own CMakeLists also
# configures test programs (GTK 3 required on Linux), adds -march=native to
# Release builds on Linux/macOS and has no install rules on Windows.
# Offline: -DOPENPNP_CAPTURE_DIR=<unzipped sources of that commit>.
option(YAB_WANT_OPENPNP_CAPTURE "Webcam input for the ST-V Print Club games (openpnp-capture)" ON)
set( kronos_qt_camera_LIBRARIES "" )
set( kronos_qt_camera_FOUND FALSE )
if ( YAB_WANT_OPENPNP_CAPTURE )
	set( OPC_COMMIT 32a9bdd3e8e3a31b12cb6573e7c6076208421651 )
	set( OPC_LIBS "" )
	set( OPC_INCLUDES "" )
	set( OPC_PLATFORM "" )
	set( OPC_PLATFORM_OK TRUE )
	if ( WIN32 )
		set( OPC_PLATFORM "Win" )
		set( OPC_LIBS strmiids ole32 oleaut32 )
	elseif ( APPLE )
		set( OPC_PLATFORM "OSX" )
		set( OPC_LIBS "-framework AVFoundation" "-framework Foundation" "-framework CoreMedia"
			"-framework CoreVideo" "-framework Accelerate" "-framework IOKit" )
	else ()
		set( OPC_PLATFORM "Linux" )
		find_package( PkgConfig QUIET )
		if ( PKG_CONFIG_FOUND )
			pkg_search_module( OPC_TURBOJPEG libturbojpeg )
		endif ()
		if ( OPC_TURBOJPEG_FOUND )
			find_package( Threads REQUIRED )
			set( OPC_LIBS ${OPC_TURBOJPEG_LINK_LIBRARIES} Threads::Threads )
			set( OPC_INCLUDES ${OPC_TURBOJPEG_INCLUDE_DIRS} )
		else ()
			message( STATUS "libturbojpeg not found (package libturbojpeg0-dev): Print Club webcam disabled" )
			set( OPC_PLATFORM_OK FALSE )
		endif ()
	endif ()

	# Sources of openpnp-capture, in this order:
	#  1. OPENPNP_CAPTURE_DIR, a folder given by hand (no download);
	#  2. the GitHub archive of the pinned commit (needs no git);
	#  3. git clone, if git is found.
	# If all fail, Kronos is built without the webcam (no configure error).
	set( OPENPNP_CAPTURE_DIR "" CACHE PATH "openpnp-capture sources (empty = download them)" )
	set( OPENPNP_CAPTURE_URL "https://github.com/openpnp/openpnp-capture/archive/${OPC_COMMIT}.tar.gz"
		CACHE STRING "Archive of openpnp-capture downloaded when OPENPNP_CAPTURE_DIR is empty" )
	mark_as_advanced( OPENPNP_CAPTURE_URL )
	if ( OPC_PLATFORM_OK )
		if ( OPENPNP_CAPTURE_DIR )
			set( OPC "${OPENPNP_CAPTURE_DIR}" )
		else ()
			set( OPC_DEPS "${CMAKE_BINARY_DIR}/_deps" )
			set( OPC "${OPC_DEPS}/openpnp-capture-${OPC_COMMIT}" )
			if ( NOT EXISTS "${OPC}/include/openpnp-capture.h" )
				file( MAKE_DIRECTORY "${OPC_DEPS}" )
				set( OPC_ARCHIVE "${OPC_DEPS}/openpnp-capture-${OPC_COMMIT}.tar.gz" )
				message( STATUS "Downloading openpnp-capture ${OPC_COMMIT}" )
				file( DOWNLOAD "${OPENPNP_CAPTURE_URL}" "${OPC_ARCHIVE}" STATUS OPC_DL TLS_VERIFY ON )
				list( GET OPC_DL 0 OPC_DL_CODE )
				if ( OPC_DL_CODE EQUAL 0 )
					# the GitHub archive holds one folder: openpnp-capture-<commit>
					execute_process( COMMAND ${CMAKE_COMMAND} -E tar xzf "${OPC_ARCHIVE}"
						WORKING_DIRECTORY "${OPC_DEPS}" RESULT_VARIABLE OPC_TAR )
				else ()
					list( GET OPC_DL 1 OPC_DL_MSG )
					message( STATUS "openpnp-capture download failed: ${OPC_DL_MSG}" )
				endif ()
				file( REMOVE "${OPC_ARCHIVE}" )
			endif ()
			if ( NOT EXISTS "${OPC}/include/openpnp-capture.h" )
				find_package( Git QUIET )
				if ( GIT_FOUND )
					message( STATUS "Cloning openpnp-capture with ${GIT_EXECUTABLE}" )
					file( REMOVE_RECURSE "${OPC}" )
					execute_process( COMMAND "${GIT_EXECUTABLE}" clone --quiet https://github.com/openpnp/openpnp-capture.git "${OPC}"
						RESULT_VARIABLE OPC_GIT )
					if ( OPC_GIT EQUAL 0 )
						execute_process( COMMAND "${GIT_EXECUTABLE}" checkout --quiet ${OPC_COMMIT}
							WORKING_DIRECTORY "${OPC}" RESULT_VARIABLE OPC_GIT )
					endif ()
				endif ()
			endif ()
		endif ()
		if ( NOT EXISTS "${OPC}/include/openpnp-capture.h" )
			message( WARNING "openpnp-capture sources not found: Kronos is built without the Print Club webcam. "
				"Download https://github.com/openpnp/openpnp-capture/archive/${OPC_COMMIT}.zip, unzip it "
				"and set OPENPNP_CAPTURE_DIR to that folder (or YAB_WANT_OPENPNP_CAPTURE=OFF to hide this message)." )
		endif ()
	endif ()

	if ( OPC_PLATFORM_OK AND EXISTS "${OPC}/include/openpnp-capture.h" )
		set( OPC_SOURCES
			${OPC}/common/libmain.cpp
			${OPC}/common/context.cpp
			${OPC}/common/logging.cpp
			${OPC}/common/stream.cpp )
		if ( WIN32 )
			list( APPEND OPC_SOURCES ${OPC}/win/platformcontext.cpp ${OPC}/win/platformstream.cpp )
		elseif ( APPLE )
			list( APPEND OPC_SOURCES ${OPC}/mac/platformcontext.mm ${OPC}/mac/platformstream.mm ${OPC}/mac/uvcctrl.mm )
		else ()
			list( APPEND OPC_SOURCES ${OPC}/linux/platformcontext.cpp ${OPC}/linux/platformstream.cpp
				${OPC}/linux/mjpeghelper.cpp ${OPC}/linux/yuvconverters.cpp )
		endif ()

		# common/version.h is generated by openpnp-capture's own CMakeLists
		set( GITVERSION "${OPC_COMMIT}" )
		configure_file( ${OPC}/cmake/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/openpnp-capture/version.h )

		add_library( openpnp-capture-static STATIC ${OPC_SOURCES} )
		set_target_properties( openpnp-capture-static PROPERTIES
			CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON )
		target_include_directories( openpnp-capture-static
			PUBLIC ${OPC}/include
			PRIVATE ${OPC}/common ${CMAKE_CURRENT_BINARY_DIR}/openpnp-capture ${OPC_INCLUDES} )
		target_compile_definitions( openpnp-capture-static
			PUBLIC OPENPNPCAPTURE_STATIC
			PRIVATE __PLATFORM__="${OPC_PLATFORM}" __BUILDTYPE__="kronos" )
		if ( MSVC )
			target_compile_definitions( openpnp-capture-static PRIVATE _CRT_SECURE_NO_WARNINGS )
		endif ()
		target_link_libraries( openpnp-capture-static PUBLIC ${OPC_LIBS} )
		set( kronos_qt_camera_LIBRARIES openpnp-capture-static )
		set( kronos_qt_camera_FOUND TRUE )
		message( STATUS "Print Club webcam: openpnp-capture ${OPC_COMMIT}" )
	endif ()
endif ()

# qt resources file
set( kronos_qt_RESOURCES resources/resources.qrc )

if ( WIN32 )
	# Windows port libraries
	set( kronos_qt_windows_LIBRARIES
		vfw32 legacy_stdio_definitions.lib )

	# resources
	set ( kronos_qt_windows_RESOURCES
		resources/icons/kronos.rc )
endif ( WIN32 )

if(APPLE)
    find_library(AUDIO_LIBRARY AudioUnit)
    set(kronos_qt_macosx_LIBRARIES ${AUDIO_LIBRARY})
endif()

# qt forms
set( kronos_qt_FORMS
	ui/UIYabause.ui
	ui/UISettings.ui
	ui/UIAbout.ui
	ui/UICheats.ui
	ui/UICheatAR.ui
	ui/UICheatRaw.ui
	ui/UICheatSearch.ui
	ui/UIBackupRam.ui
	ui/UIPortManager.ui
	ui/UIPadSetting.ui
	ui/UISTVSetting.ui
	ui/UIPatocarSetting.ui
	ui/UIMahjongSetting.ui
	ui/UI3DControlPadSetting.ui
    ui/UIWheelSetting.ui
    ui/UIMissionStickSetting.ui
    ui/UIDoubleMissionStickSetting.ui
	ui/UIGunSetting.ui
	ui/UIMouseSetting.ui
    ui/UIDebugCPU.ui
    ui/UIDebugSCSP.ui
    ui/UIDebugSCSPDSP.ui
    ui/UIDebugVDP1.ui
    ui/UIDebugVDP2.ui
    ui/UIDebugVDP2Viewer.ui
    ui/UIHexInput.ui
    ui/UIMemoryTransfer.ui
    ui/UIMemoryEditor.ui
    ui/UIMemorySearch.ui )

# pure C headers
set( kronos_qt_HEADERS
	QtYabause.h
	CommonDialogs.h
	PerQt.h
	KeyboardQt.h
	ControllerTypes.h
	Arguments.h )

# C headers needing moc
set( kronos_qt_MOC_HEADERS
	ui/UIYabause.h
	ui/UISettings.h
	ui/UIAbout.h
	ui/UICheats.h
	ui/UICheatAR.h
	ui/UICheatRaw.h
	ui/UICheatSearch.h
	ui/UIBackupRam.h
	ui/UIPortManager.h
	ui/UIControllerSetting.h
	ui/UIPadSetting.h
        ui/UISTVSetting.h
	ui/UIPatocarSetting.h
	ui/UIMahjongSetting.h
	ui/UI3DControlPadSetting.h
    ui/UIWheelSetting.h
    ui/UIMissionStickSetting.h
    ui/UIDoubleMissionStickSetting.h
	ui/UIGunSetting.h
	ui/UIMouseSetting.h
	ui/UIButtonListSetting.h
	ui/UIShortcutManager.h
    ui/UIDebugCPU.h
    ui/UIDebugM68K.h
    ui/UIDebugSH2Profile.h
    ui/UIDebugSCSP.h
    ui/UIDebugSCSPChan.h
    ui/UIDebugSCSPDSP.h
    ui/UIDebugSCUDSP.h
    ui/UIDebugSH2.h
    ui/UIDebugVDP1.h
    ui/UIEditsExport.h
    ui/UIDebugGfxEditor.h
    ui/UIDebugTextEditor.h
    ui/UIMovieEditor.h
    ui/UIRamWatch.h
	TextTools.h
    ui/UIDebugVDP2.h
    ui/UIDebugVDP2Viewer.h
    ui/UIDisasm.h
    ui/UIHexInput.h
    ui/UIMemoryTransfer.h
    ui/UIHexEditor.h
    ui/UIMemoryEditor.h
    ui/UIMemorySearch.h
	YabauseGL.h
	VolatileSettings.h
	Settings.h
	CameraCapture.h
	LuaScript.h
	ui/UILuaConsole.h
	YabauseThread.h )

# C sources
set( kronos_qt_SOURCES
	main.cpp
	QtYabause.cpp
	CommonDialogs.cpp
	EditsPatcher.cpp
	TextTools.cpp
	PerQt.c
	KeyboardQt.cpp
	ui/UIYabause.cpp
	ui/UISettings.cpp
	ui/UIAbout.cpp
	ui/UICheats.cpp
	ui/UICheatAR.cpp
	ui/UICheatRaw.cpp
	ui/UICheatSearch.cpp
	ui/UIBackupRam.cpp
	ui/UIPortManager.cpp
	ui/UIControllerSetting.cpp
	ui/UIPadSetting.cpp
        ui/UISTVSetting.cpp
	ui/UIPatocarSetting.cpp
	ui/UIMahjongSetting.cpp
	ui/UI3DControlPadSetting.cpp
    ui/UIWheelSetting.cpp
    ui/UIMissionStickSetting.cpp
    ui/UIDoubleMissionStickSetting.cpp
	ui/UIGunSetting.cpp
	ui/UIMouseSetting.cpp
	ui/UIButtonListSetting.cpp
	ui/UIShortcutManager.cpp
    ui/UIDebugCPU.cpp
    ui/UIDebugM68K.cpp
    ui/UIDebugSH2Profile.cpp
    ui/UIDebugSCSP.cpp
    ui/UIDebugSCSPChan.cpp
    ui/UIDebugSCSPDSP.cpp
    ui/UIDebugSCUDSP.cpp
    ui/UIDebugSH2.cpp
    ui/UIDebugVDP1.cpp
    ui/UIEditsExport.cpp
    ui/UIDebugGfxEditor.cpp
    ui/UIDebugTextEditor.cpp
    ui/UIMovieEditor.cpp
    ui/UIRamWatch.cpp
    ui/UIDebugVDP2.cpp
    ui/UIDebugVDP2Viewer.cpp
    ui/UIDisasm.cpp
    ui/UIHexInput.cpp
    ui/UIMemoryTransfer.cpp
    ui/UIHexEditor.cpp
    ui/UIMemoryEditor.cpp
    ui/UIMemorySearch.cpp
	Settings.cpp
	VolatileSettings.cpp
	YabauseThread.cpp
	CameraCapture.cpp
	Arguments.cpp )

	set( kronos_qt_SOURCES ${kronos_qt_SOURCES} YabauseGL.cpp )

if(Qt5_FOUND)
	QT5_ADD_RESOURCES( kronos_qt_RCC_RESOURCES ${kronos_qt_RESOURCES} )
	QT5_WRAP_UI( kronos_qt_UI_FORMS ${kronos_qt_FORMS} )
	QT5_WRAP_CPP( kronos_qt_MOC_SOURCES ${kronos_qt_MOC_HEADERS} )
endif()

set( kronos_qt_SOURCES ${kronos_qt_SOURCES} resources/icons/kronos.icns )
set_source_files_properties( resources/icons/kronos.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources )

add_executable( kronos-qt WIN32
	${kronos_qt_windows_SOURCES}
	${kronos_qt_windows_RESOURCES}
	${kronos_qt_RCC_RESOURCES}
	${kronos_qt_HEADERS}
	${kronos_qt_MOC_HEADERS}
	${kronos_qt_SOURCES}
	${kronos_qt_UI_FORMS}
	${kronos_qt_MOC_SOURCES} )

if (Qt5_FOUND)
target_link_libraries(kronos-qt Qt5::OpenGL)
target_link_libraries(kronos-qt Qt5::Widgets)
target_link_libraries(kronos-qt Qt5::Multimedia)
include_directories(
	.
	${CMAKE_CURRENT_BINARY_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/ui
	${Qt5Widgets_INCLUDE_DIRS}
	${Qt5OpenGL_INCLUDE_DIRS}
	${Qt5Multimedia_INCLUDE_DIRS} )
endif()

# APPLE // not necessary mac os x, but i don't care ;)
if (APPLE)
	set_target_properties( kronos-qt PROPERTIES MACOSX_BUNDLE true )
	set_target_properties( kronos-qt PROPERTIES MACOSX_BUNDLE_ICON_FILE kronos.icns )
	set_target_properties( kronos-qt PROPERTIES MACOSX_BUNDLE_LONG_VERSION_STRING "${YAB_VERSION}" )
	set_target_properties( kronos-qt PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME Kronos )
	set_target_properties( kronos-qt PROPERTIES MACOSX_BUNDLE_SHORT_VERSION_STRING "${YAB_VERSION}" )
	set_target_properties( kronos-qt PROPERTIES MACOSX_BUNDLE_COPYRIGHT "(c) Kronos Team" )
endif (APPLE)

# Lua 5.4 (scripting console), embedded sources: tools/lua
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/lua ${CMAKE_CURRENT_BINARY_DIR}/lua )
target_link_libraries( kronos-qt kronos-lua )
target_link_libraries( kronos-qt kronos )
target_link_libraries( kronos-qt ${KRONOS_LIBRARIES} )
target_link_libraries( kronos-qt ${PORT_LIBRARIES} )
target_link_libraries( kronos-qt ${kronos_qt_windows_LIBRARIES} )
target_link_libraries( kronos-qt ${kronos_qt_macosx_LIBRARIES} )
target_link_libraries( kronos-qt ${kronos_qt_camera_LIBRARIES} )
if ( kronos_qt_camera_FOUND )
	target_compile_definitions( kronos-qt PRIVATE HAVE_OPENPNP_CAPTURE=1 )
endif ()
if (Qt5_FOUND)
	target_link_libraries( kronos-qt ${Qt5Widgets_LIBRARIES} ${Qt5OpenGL_LIBRARIES} ${Qt5Core_QTMAIN_LIBRARIES} ${Qt5Multimedia_LIBRARIES})
endif()

yab_port_success(kronos-qt)
configure_file(kronos.desktop.in ${YAB_PORT_NAME}.desktop)

if (WIN32)
	set(QT_BINARY_DIR "${Qt5_Dir}/bin")
	install(TARGETS kronos-qt DESTINATION ".")
	if (SDL_FOUND)
		get_filename_component(SDL_BINARY_DIR ${SDL_LIBRARY} PATH)
		install(DIRECTORY ${SDL_BINARY_DIR}/
			DESTINATION "."
			FILES_MATCHING PATTERN "SDL.dll")
	endif ()
	if (SDL2_FOUND)
		if (NOT MSYS2_BUILD)
			get_filename_component(SDL2_BINARY_DIR ${SDL2_LIBRARY} PATH)
			install(DIRECTORY ${SDL2_BINARY_DIR}/
				DESTINATION "."
				FILES_MATCHING PATTERN "SDL2.dll")
		else ()
			install(FILES ${QT_BINARY_DIR}/SDL2.dll DESTINATION ".")
		endif ()
	endif ()
	if(Qt5_FOUND)
    get_target_property(_qt5_qmake_location Qt5::qmake IMPORTED_LOCATION)

    execute_process(
        COMMAND "${_qt5_qmake_location}" -query QT_INSTALL_PREFIX
        RESULT_VARIABLE return_code
        OUTPUT_VARIABLE qt5_install_prefix
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
#	install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../AUTHORS    DESTINATION "." RENAME AUTHORS.txt)
    set(imported_location "${qt5_install_prefix}/bin/windeployqt.exe")
#	install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../ChangeLog  DESTINATION "." RENAME ChangeLog.txt)
    if(EXISTS ${imported_location})
        add_executable(Qt5::windeployqt IMPORTED)
#	install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../COPYING    DESTINATION "." RENAME COPYING.txt)
        set_target_properties(Qt5::windeployqt PROPERTIES
            IMPORTED_LOCATION ${imported_location}
        )
    endif()
	endif()
#	install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../README     DESTINATION "." RENAME README.txt)
  if(TARGET Qt5::windeployqt)
#	install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../README.WIN DESTINATION "." RENAME README.WIN.txt)
    add_custom_command(TARGET kronos-qt
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_CURRENT_BINARY_DIR}/windeployqt"
        COMMAND set PATH=%PATH%$<SEMICOLON>${qt5_install_prefix}/bin
        COMMAND Qt5::windeployqt --dir "${CMAKE_CURRENT_BINARY_DIR}/windeployqt" "$<TARGET_FILE_DIR:kronos-qt>/$<TARGET_FILE_NAME:kronos-qt>"
    )

    install(
        DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/windeployqt/"
				DESTINATION "."
    )
		endif()

  if (MSVC)
    set (CMAKE_EXE_LINKER_FLAGS /SAFESEH:NO )
  endif (MSVC)
else ()
	install(TARGETS kronos-qt DESTINATION "bin")
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${YAB_PORT_NAME}.desktop DESTINATION "share/applications")
	install(FILES "doc/kronos.1" DESTINATION "${YAB_MAN_DIR}/man1" RENAME "${YAB_PORT_NAME}.1")
	install(FILES "resources/icons/kronos.png" DESTINATION "share/pixmaps")
endif ()
