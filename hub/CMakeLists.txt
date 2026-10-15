# zchat_hub: the 3D hub (see hub.cpp), built when ZCHAT_HUB is on. One compilation unit (win32_hub.cpp includes the
# rest), the models of assets/ built into it, and nothing but the system's libraries. Windows only, for now.

add_executable(zchat_hub WIN32 win32_hub.cpp)
# The other sources are part of win32_hub.cpp: listed for the editors, not compiled on their own.
set(zchat_hub_parts hub.cpp hub_render.cpp hub_bowling.cpp hub_math.h hub_gl.h hub_platform.h assets/layout.h)
target_sources(zchat_hub PRIVATE ${zchat_hub_parts})
set_source_files_properties(${zchat_hub_parts} PROPERTIES HEADER_FILE_ONLY TRUE)

# The models, made by assets/make_assets.py with Blender, as headers.
set(zchat_hub_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
foreach(model plaza crewmate pin ball shadow)
    add_custom_command(
        OUTPUT "${zchat_hub_generated}/${model}_mesh.hpp"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/assets/${model}.mesh"
                "-DOUTPUT=${zchat_hub_generated}/${model}_mesh.hpp" -DNAME=${model}_mesh
                -P "${PROJECT_SOURCE_DIR}/cmake/embed.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/${model}.mesh" "${PROJECT_SOURCE_DIR}/cmake/embed.cmake"
        VERBATIM)
    target_sources(zchat_hub PRIVATE "${zchat_hub_generated}/${model}_mesh.hpp")
endforeach()
target_include_directories(zchat_hub PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/assets"
                                             "${zchat_hub_generated}")

# zchat's icon, on the window too.
enable_language(RC)
file(WRITE "${zchat_hub_generated}/zchat_hub.rc.in" "32512 ICON \"${PROJECT_SOURCE_DIR}/extras/icons/zchat-icon.ico\"\n")
configure_file("${zchat_hub_generated}/zchat_hub.rc.in" "${zchat_hub_generated}/zchat_hub.rc" COPYONLY)
target_sources(zchat_hub PRIVATE "${zchat_hub_generated}/zchat_hub.rc")

target_link_libraries(zchat_hub PRIVATE opengl32 gdi32 user32)
target_compile_definitions(zchat_hub PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if (MSVC)
    target_compile_options(zchat_hub PRIVATE /W4 /utf-8)
else()
    target_compile_options(zchat_hub PRIVATE -Wall -Wextra)
    target_link_options(zchat_hub PRIVATE -static -municode)
endif()
