cmake_minimum_required(VERSION 3.25)

project(CyberCraft VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# RED4ext.SDK (header-only: nothing to link, the plugin resolves game addresses through RED4ext)
set(RED4EXT_HEADER_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(extern/RED4ext.SDK EXCLUDE_FROM_ALL)

file(GLOB_RECURSE CYBERCRAFT_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)

add_library(CyberCraft SHARED ${CYBERCRAFT_SOURCES})
target_include_directories(CyberCraft PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
target_precompile_headers(CyberCraft PRIVATE src/PCH.h)
target_link_libraries(CyberCraft PRIVATE RED4ext::SDK d3d12.lib dxgi.lib d3dcompiler.lib ole32.lib oleaut32.lib shell32.lib)
target_compile_definitions(CyberCraft PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN CYBERCRAFT_VERSION="${PROJECT_VERSION}"
	CYBERCRAFT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} CYBERCRAFT_VERSION_MINOR=${PROJECT_VERSION_MINOR} CYBERCRAFT_VERSION_PATCH=${PROJECT_VERSION_PATCH})
if(MSVC)
	target_compile_options(CyberCraft PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8 /MP /wd4200 /wd4100)
	target_link_options(CyberCraft PRIVATE /DEBUG:FULL)
endif()

# Development: copy the built DLL (and the redscript files) straight into the game,
# -DCYBERCRAFT_DEPLOY_DIR="<game>/red4ext/plugins/CyberCraft".
if(CYBERCRAFT_DEPLOY_DIR)
	add_custom_command(TARGET CyberCraft POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${CYBERCRAFT_DEPLOY_DIR}/scripts"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:CyberCraft>" "$<TARGET_PDB_FILE:CyberCraft>" "${CYBERCRAFT_DEPLOY_DIR}"
		COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/scripts" "${CYBERCRAFT_DEPLOY_DIR}/scripts"
		# CyberCraft.archive (cybercraft\empty.ent, what lights and colliders are spawned from) goes
		# where the game loads mod archives: <game>/archive/pc/mod.
		COMMAND ${CMAKE_COMMAND} -E make_directory "${CYBERCRAFT_DEPLOY_DIR}/../../../archive/pc/mod"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/archive/CyberCraft.archive" "${CYBERCRAFT_DEPLOY_DIR}/../../../archive/pc/mod"
		VERBATIM)
endif()
