cmake_minimum_required(VERSION 3.20)
# Runtime framework forked from the author's MIT-licensed shrek2-team-action-web port.
project(detectivebarbie C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(EMSCRIPTEN)
  # Every object must use shared memory (the game runs on pthreads).
  add_compile_options(-pthread)
  add_link_options(-pthread)
endif()

option(DETECTIVE_NO_GAME_CODE "Build only the runtime into libdetective_runtime.a: no recompiled game code, no main(), no SDL link (for building before the translator is ready)" OFF)
option(RECOMP_STACK_CHECK "Enable guest stack-balance diagnostics (pair with gen-c --stack-check)" OFF)

# HLE runtime sources: everything the translated game can call, with no game code inside.
set(RUNTIME_SOURCES
  src/cpu_helpers.c
  src/core/core.cpp
  src/core/memory.cpp
  src/core/objects.cpp
  src/core/resources.cpp
  src/hle/kernel32.cpp
  src/hle/kernel32_files.cpp
  src/hle/advapi32.cpp
  src/hle/user32.cpp
  src/hle/smooth.cpp
  src/hle/com.cpp
  src/hle/dsound.cpp
  src/audio/mixer.cpp)

# Wave-2 worker sources: appended only when the file exists, so the tree always configures even
# while those workers are still in flight.
foreach(extra src/hle/ddraw.cpp src/hle/win32_extra.cpp)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${extra}")
    list(APPEND RUNTIME_SOURCES ${extra})
  endif()
endforeach()

if(DETECTIVE_NO_GAME_CODE)
  add_library(detective_runtime STATIC ${RUNTIME_SOURCES})
  target_include_directories(detective_runtime PRIVATE include src)
  target_compile_options(detective_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(NOT EMSCRIPTEN)
    target_link_libraries(detective_runtime PUBLIC pthread m)
  endif()
  if(RECOMP_TRACE)
    target_compile_definitions(detective_runtime PUBLIC RECOMP_TRACE=1)
  endif()
  if(RECOMP_STACK_CHECK)
    target_compile_definitions(detective_runtime PUBLIC RECOMP_STACK_CHECK=1)
  endif()
  add_executable(import_coverage tests/import_coverage.cpp tests/plat_stubs.cpp)
  target_include_directories(import_coverage PRIVATE include src)
  target_compile_options(import_coverage PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(import_coverage PRIVATE detective_runtime)
  return()
endif()

# Generated, translated game code (never committed; produced by recomp/src/cli.ts gen-c).
set(GEN_DIR "${CMAKE_SOURCE_DIR}/../work/gen-c" CACHE PATH "Directory of recompiled C sources")
set(PREBUILT_RECOMP_GAME "" CACHE FILEPATH "Optional path to prebuilt librecomp_game.a")
if(PREBUILT_RECOMP_GAME AND EXISTS "${PREBUILT_RECOMP_GAME}")
  add_library(recomp_game STATIC IMPORTED)
  set_target_properties(recomp_game PROPERTIES
    IMPORTED_LOCATION "${PREBUILT_RECOMP_GAME}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include;${GEN_DIR}")
else()
  set(GEN_OPT "-O1" CACHE STRING "Optimisation flags for the recompiled code")
  file(GLOB GEN_SOURCES "${GEN_DIR}/funcs_*.c" "${GEN_DIR}/recomp_table.c")
  if(NOT GEN_SOURCES)
    message(FATAL_ERROR "No generated sources in ${GEN_DIR}: run node recomp/src/cli.ts gen-c first")
  endif()

  add_library(recomp_game STATIC ${GEN_SOURCES})
  target_include_directories(recomp_game PUBLIC include ${GEN_DIR})
  separate_arguments(GEN_OPT_LIST UNIX_COMMAND "${GEN_OPT}")
  target_compile_options(recomp_game PRIVATE -w ${GEN_OPT_LIST})
  if(RECOMP_TRACE)
    target_compile_definitions(recomp_game PUBLIC RECOMP_TRACE=1)
  endif()
  if(RECOMP_STACK_CHECK)
    target_compile_definitions(recomp_game PUBLIC RECOMP_STACK_CHECK=1)
  endif()
endif()

add_library(dlmalloc STATIC ../third_party/dlmalloc/malloc.c)
target_compile_definitions(dlmalloc PRIVATE ONLY_MSPACES=1 MSPACES=1 USE_LOCKS=1 HAVE_MMAP=0 HAVE_MORECORE=0)
target_compile_options(dlmalloc PRIVATE -w -O2)

if(EMSCRIPTEN)
  add_executable(detectivebarbie ${RUNTIME_SOURCES} src/platform/platform_web.cpp src/web/web_main.cpp)
  target_include_directories(detectivebarbie PRIVATE include src)
  target_compile_options(detectivebarbie PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(detectivebarbie PRIVATE recomp_game dlmalloc)
  set(WEB_EXPORTS "_main,_malloc,_free,_web_render_init,_web_render_tick,_web_audio_ring_create,_web_canvas_size,_web_key,_web_mouse_move,_web_mouse_button,_web_mouse_wheel,_web_focus,_web_client_size")
  target_link_options(detectivebarbie PRIVATE
    -sGLOBAL_BASE=1073741824
    -sINITIAL_MEMORY=2147483648
    -sALLOW_MEMORY_GROWTH=0
    -sSTACK_SIZE=4194304
    -sDEFAULT_PTHREAD_STACK_SIZE=4194304
    -sPROXY_TO_PTHREAD=1
    -sPTHREAD_POOL_SIZE=8
    -sWASMFS=1
    -sFORCE_FILESYSTEM=1
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createDetectiveBarbie
    -sINVOKE_RUN=0
    -sENVIRONMENT=web,worker
    -sEXPORTED_FUNCTIONS=${WEB_EXPORTS}
    -sEXPORTED_RUNTIME_METHODS=callMain,ccall,cwrap,HEAPU8,HEAPU32,HEAPF32,stringToNewUTF8,ENV
    -sASSERTIONS=1)

  # Player build (deployable): game translated in the browser; loaded as a separate module.
  add_executable(detective_player ${RUNTIME_SOURCES} src/platform/platform_web.cpp src/web/web_main.cpp
    ${CMAKE_SOURCE_DIR}/../web/runtime/web_game.cpp)
  target_include_directories(detective_player PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(detective_player PRIVATE RT_PLAYER=1)
  target_include_directories(detective_player PRIVATE include src)
  target_compile_options(detective_player PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(detective_player PRIVATE dlmalloc)
  set(PLAYER_EXPORTS "${WEB_EXPORTS},_web_game_alloc,_web_game_register,_web_game_import,_web_game_bytes,_web_game_len,_web_game_index")
  get_target_property(WEB_LINK_OPTIONS detectivebarbie LINK_OPTIONS)
  list(TRANSFORM WEB_LINK_OPTIONS REPLACE "^-sEXPORTED_FUNCTIONS=.*" "-sEXPORTED_FUNCTIONS=${PLAYER_EXPORTS}")
  list(TRANSFORM WEB_LINK_OPTIONS REPLACE "^-sASSERTIONS=1$" "-sASSERTIONS=0")
  target_link_options(detective_player PRIVATE ${WEB_LINK_OPTIONS} -sALLOW_TABLE_GROWTH=1)
else()
  find_package(SDL2 REQUIRED)
  add_executable(detective ${RUNTIME_SOURCES} src/main.cpp src/platform/platform_sdl.cpp)
  target_include_directories(detective PRIVATE include src)
  target_compile_options(detective PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(RECOMP_STACK_CHECK)
    target_compile_definitions(detective PRIVATE RECOMP_STACK_CHECK=1)
  endif()
  target_link_libraries(detective PRIVATE recomp_game dlmalloc SDL2::SDL2 pthread m)
  # Web/native parity name kept for emscripten builds above.
  add_executable(detectivebarbie ALIAS detective)
endif()
