cmake_minimum_required(VERSION 3.22)

project(tools-scenes VERSION 1.4.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Tools Scenes: plugin de OBS Studio (31+) con vista previa de escenas tipo vMix.
#
# No usa moc/uic/rcc (sin Q_OBJECT), por eso basta con las cabeceras de Qt y
# las bibliotecas de importación: se puede compilar de forma cruzada.
#
#   OBS_SOURCE_DIR  Árbol fuente de obs-studio (cabeceras de libobs y frontend)
#   QT_DIR          Qt6 de obs-deps para la plataforma destino
#   OBS_SIMDE_DIR   (macOS/Linux) carpeta que contiene simde/ (obs-deps/include)
#   OBS_APP_DIR     (macOS) OBS.app contra el que se enlaza
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(OBS_SOURCE_DIR "" CACHE PATH "obs-studio source tree")
set(QT_DIR "" CACHE PATH "Qt6 (obs-deps) for the target platform")
option(SCENE_PREVIO_DEBUG "Diagnóstico de pruebas (no usar en producción)" OFF)

if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs.h")
  message(FATAL_ERROR "OBS_SOURCE_DIR no contiene libobs/obs.h")
endif()

set(SOURCES
  src/plugin-main.cpp
  src/live-engine.cpp
  src/live-engine.hpp
  src/media-dock.cpp
  src/media-dock.hpp
  src/output-dock.cpp
  src/output-dock.hpp
  src/projector-window.cpp
  src/projector-window.hpp
  src/transform-editor.cpp
  src/transform-editor.hpp
  src/thumbnail-loader.cpp
  src/thumbnail-loader.hpp
  src/media-card.cpp
  src/media-card.hpp
  src/flow-layout.hpp
  src/layer-strip.cpp
  src/layer-strip.hpp
  src/wall-canvas.cpp
  src/wall-canvas.hpp
  src/wall-dock.cpp
  src/wall-dock.hpp)

add_library(tools-scenes MODULE ${SOURCES})

target_compile_definitions(tools-scenes PRIVATE
  PLUGIN_VERSION="${PROJECT_VERSION}"
  QT_NO_DEBUG QT_CORE_LIB QT_GUI_LIB QT_WIDGETS_LIB
  QT_NO_KEYWORDS)
if(SCENE_PREVIO_DEBUG)
  target_compile_definitions(tools-scenes PRIVATE SCENE_PREVIO_DEBUG)
endif()

target_include_directories(tools-scenes PRIVATE
  "${OBS_SOURCE_DIR}/libobs"
  "${OBS_SOURCE_DIR}/UI/obs-frontend-api")

if(WIN32)
  # ------------------------------------------------------------- Windows ----
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  set_property(TARGET tools-scenes PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  set_target_properties(tools-scenes PROPERTIES PREFIX "" SUFFIX ".dll")

  # _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR: evita el fallo conocido de std::mutex
  # cuando el PC tiene un msvcp140.dll anterior a VS 2022 17.10.
  target_compile_definitions(tools-scenes PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
  target_compile_options(tools-scenes PRIVATE /utf-8 /EHsc /Zc:__cplusplus /permissive- /W3 /O2)

  target_include_directories(tools-scenes PRIVATE
    "${QT_DIR}/include"
    "${QT_DIR}/include/QtCore"
    "${QT_DIR}/include/QtGui"
    "${QT_DIR}/include/QtWidgets")

  # Bibliotecas de importación de obs.dll / obs-frontend-api.dll generadas a
  # partir de .def (solo los símbolos que usa el plugin).
  set(IMPORT_DIR "${CMAKE_CURRENT_BINARY_DIR}/import")
  file(MAKE_DIRECTORY "${IMPORT_DIR}")
  foreach(lib obs obs-frontend-api)
    add_custom_command(
      OUTPUT "${IMPORT_DIR}/${lib}.lib"
      COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/${lib}.def"
              "/out:${IMPORT_DIR}/${lib}.lib"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/${lib}.def"
      VERBATIM)
    list(APPEND IMPORT_LIBS "${IMPORT_DIR}/${lib}.lib")
  endforeach()
  add_custom_target(tools-scenes-imports DEPENDS ${IMPORT_LIBS})
  add_dependencies(tools-scenes tools-scenes-imports)

  target_link_libraries(tools-scenes PRIVATE
    ${IMPORT_LIBS}
    "${QT_DIR}/lib/Qt6Core.lib"
    "${QT_DIR}/lib/Qt6Gui.lib"
    "${QT_DIR}/lib/Qt6Widgets.lib"
    user32 shell32 ole32 gdi32)
  target_link_options(tools-scenes PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)

  install(TARGETS tools-scenes LIBRARY DESTINATION "tools-scenes/bin/64bit" RUNTIME DESTINATION "tools-scenes/bin/64bit")
  install(DIRECTORY data/ DESTINATION "tools-scenes/data")

elseif(APPLE)
  # --------------------------------------------------------------- macOS ----
  set(OBS_APP_DIR "/Applications/OBS.app" CACHE PATH "OBS.app")
  set(OBS_SIMDE_DIR "" CACHE PATH "Directory containing simde/")
  set(FW "${OBS_APP_DIR}/Contents/Frameworks")

  set_target_properties(tools-scenes PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    MACOSX_BUNDLE_BUNDLE_NAME tools-scenes
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.japalacioo.tools-scenes
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    INSTALL_RPATH "@executable_path/../Frameworks"
    BUILD_WITH_INSTALL_RPATH TRUE)

  target_include_directories(tools-scenes PRIVATE
    "${OBS_SIMDE_DIR}"
    "${QT_DIR}/lib/QtCore.framework/Headers"
    "${QT_DIR}/lib/QtGui.framework/Headers"
    "${QT_DIR}/lib/QtWidgets.framework/Headers")
  # Cabeceras Qt desde obs-deps; binarios Qt desde OBS.app.
  target_compile_options(tools-scenes PRIVATE
    "-F${QT_DIR}/lib"
    "SHELL:-iframework ${QT_DIR}/lib"
    -Wall -Wno-unused-parameter)
  # OBS_MAC_STUBS: carpeta con stubs .tbd de libobs y obs-frontend-api para
  # arm64 + x86_64 (los genera scripts/build-macos.sh). Permite compilar un
  # binario universal aunque el OBS.app local sea de una sola arquitectura.
  set(OBS_MAC_STUBS "" CACHE PATH "Stubs .tbd de libobs/obs-frontend-api")
  if(OBS_MAC_STUBS)
    set(OBS_LIB_F "${OBS_MAC_STUBS}")
    set(OBS_FRONTEND_LIB "${OBS_MAC_STUBS}/obs-frontend-api.tbd")
  else()
    set(OBS_LIB_F "${FW}")
    set(OBS_FRONTEND_LIB "${FW}/obs-frontend-api.dylib")
  endif()
  target_link_options(tools-scenes PRIVATE
    "-F${OBS_LIB_F}"
    "-F${FW}"
    "SHELL:-framework libobs"
    "SHELL:-framework QtCore"
    "SHELL:-framework QtGui"
    "SHELL:-framework QtWidgets"
    "${OBS_FRONTEND_LIB}")

  file(GLOB_RECURSE DATA_FILES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/data" "${CMAKE_CURRENT_SOURCE_DIR}/data/*")
  foreach(f ${DATA_FILES})
    get_filename_component(sub "${f}" DIRECTORY)
    target_sources(tools-scenes PRIVATE "data/${f}")
    set_source_files_properties("data/${f}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/${sub}")
  endforeach()
endif()
