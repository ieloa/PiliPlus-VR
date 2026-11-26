cmake_minimum_required(VERSION 3.22)
project(p5m)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENXR_DIR "C:/Users/al200/.gemini/antigravity/scratch/openxr_aar" CACHE PATH "Path to openxr_aar")

add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION "${OPENXR_DIR}/prefab/modules/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENXR_DIR}/prefab/modules/headers/include"
)

add_library(p5m-vr SHARED
        pilha_nativa.cpp
        p5m_jni.cpp
        spatializer.cpp
        tone_mapper.cpp
        tone_mapper_ai.cpp
        tone_mapper_offsets.cpp
        xr_session.cpp)

target_include_directories(p5m-vr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_library(ANDROID_LIB_LOG log)
find_library(ANDROID_LIB_ANDROID android)
find_library(ANDROID_LIB_EGL EGL)
find_library(ANDROID_LIB_GLESV3 GLESv3)
find_library(ANDROID_LIB_JNIGRAPHICS jnigraphics)

target_link_libraries(p5m-vr
        openxr_loader
        "${ANDROID_LIB_LOG}"
        "${ANDROID_LIB_ANDROID}"
        "${ANDROID_LIB_EGL}"
        "${ANDROID_LIB_GLESV3}"
        "${ANDROID_LIB_JNIGRAPHICS}")
