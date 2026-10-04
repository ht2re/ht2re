cmake_minimum_required(VERSION 3.16)
project(ht2re CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
# SDL2_mixer is optional: audio degrades gracefully when it is missing
pkg_check_modules(SDL2_MIXER sdl2_mixer)
message(STATUS "SDL2: ${SDL2_LINK_LIBRARIES} (${SDL2_VERSION})")

find_package(OpenGL REQUIRED)

add_executable(ht2re
    src/main.cpp
    src/app/App.cpp
    src/game/Vehicle.cpp
    src/game/Collision.cpp
    src/game/Traffic.cpp
    src/game/Economy.cpp
    src/game/Cockpit.cpp
    src/audio/Audio.cpp
    src/ui/Video.cpp
    src/ui/Loading.cpp
    src/app/Input.cpp
    src/script/Sdte.cpp
    src/script/Sexp.cpp
    src/script/GameData.cpp
    src/script/ScriptRules.cpp
    src/gfx/Renderer.cpp
    src/gfx/Mirror.cpp
    src/res/Res.cpp
    src/res/Mask.cpp
    src/ui/Font.cpp
    src/ui/Menu.cpp
    src/ui/EscMenu.cpp
    src/ui/Hud.cpp
    src/world/Terrain.cpp
    src/world/Way.cpp
    src/world/B3D.cpp
    src/world/SceneTextures.cpp
    src/world/GroundMap.cpp
    src/world/Weather.cpp
    src/ui/Map.cpp
)

target_include_directories(ht2re PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_MIXER_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIR}
)

target_link_directories(ht2re PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2_MIXER_LIBRARY_DIRS})
if(SDL2_MIXER_FOUND)
    message(STATUS "SDL2_mixer: ${SDL2_MIXER_LIBRARIES} - audio enabled")
    target_compile_definitions(ht2re PRIVATE HT2_HAVE_SDL_MIXER=1)
else()
    message(STATUS "SDL2_mixer not found - audio disabled")
endif()

target_link_libraries(ht2re PRIVATE
    ${SDL2_LIBRARIES}
    ${SDL2_MIXER_LIBRARIES}
    ${OPENGL_LIBRARIES}
)

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(ht2re PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()