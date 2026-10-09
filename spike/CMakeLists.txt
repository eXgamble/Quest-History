cmake_minimum_required(VERSION 3.21)
project(QuestHistorySpike VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Throwaway spike: borrows Death Timer's CommonLibSSE-NG checkout (ng v10.0.0, AE only).
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(COMMONLIB_DIR "C:/Modding/houseCARL/DeathTimer/skse/lib/commonlibsse-ng")
add_subdirectory(${COMMONLIB_DIR} commonlib)
if(NOT COMMAND add_commonlibsse_plugin)
	include(${COMMONLIB_DIR}/cmake/CommonLibSSE.cmake)
endif()

add_commonlibsse_plugin(${PROJECT_NAME}
	NAME "QuestHistorySpike"
	AUTHOR "eXgamble"
	SOURCES src/plugin.cpp)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE /Zc:preprocessor /permissive- /EHsc /utf-8 /W4)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

set(DEPLOY_DIR "C:/Modding/MO2/mods/QuestHistorySpike/SKSE/Plugins")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DEPLOY_DIR}"
	VERBATIM)
