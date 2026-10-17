cmake_minimum_required(VERSION 3.22)
project(XTRACT VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- JUCE: reuse the WXRST checkout next door if present, else fetch ----
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../VST1/JUCE/CMakeLists.txt")
    add_subdirectory(../VST1/JUCE JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.10
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---- ONNX Runtime (prebuilt) for the Basic Pitch note transcriber ----
include(FetchContent)
set(ORT_VER 1.20.1)
if(APPLE)
    FetchContent_Declare(onnxruntime
        URL https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VER}/onnxruntime-osx-universal2-${ORT_VER}.tgz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
elseif(WIN32)
    FetchContent_Declare(onnxruntime
        URL https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VER}/onnxruntime-win-x64-${ORT_VER}.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
else()
    FetchContent_Declare(onnxruntime
        URL https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VER}/onnxruntime-linux-x64-${ORT_VER}.tgz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()
FetchContent_MakeAvailable(onnxruntime)
set(ORT_DIR ${onnxruntime_SOURCE_DIR})
if(APPLE)
    set(ORT_LIB ${ORT_DIR}/lib/libonnxruntime.${ORT_VER}.dylib)
    set(ORT_RUNTIME_FILE ${ORT_LIB})
elseif(WIN32)
    set(ORT_LIB ${ORT_DIR}/lib/onnxruntime.lib)
    set(ORT_RUNTIME_FILE ${ORT_DIR}/lib/onnxruntime.dll)
else()
    set(ORT_LIB ${ORT_DIR}/lib/libonnxruntime.so)
    set(ORT_RUNTIME_FILE ${ORT_LIB})
endif()

# ---- demucs.cpp (MIT, sevagh) + Eigen: stem separation ----
file(GLOB DEMUCS_SOURCES third_party/demucs/*.cpp)
add_library(demucs STATIC ${DEMUCS_SOURCES})
target_include_directories(demucs PUBLIC third_party/demucs third_party/eigen)
target_compile_definitions(demucs PUBLIC NDEBUG EIGEN_MPL2_ONLY)
set_target_properties(demucs PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(demucs PRIVATE /O2 /fp:fast /bigobj /wd4244 /wd4267 /wd4305)
else()
    target_compile_options(demucs PRIVATE -O3 -ffast-math -fno-finite-math-only -w)
endif()
if(APPLE)
    target_compile_definitions(demucs PUBLIC EIGEN_USE_BLAS)
    target_link_libraries(demucs PUBLIC "-framework Accelerate")
endif()

# ---- basicpitch.cpp (MIT, sevagh): audio -> MIDI via ONNX Runtime ----
add_library(basicpitch STATIC
    third_party/basicpitch/ort_inference.cpp
    third_party/basicpitch/midi_notes.cpp
    third_party/basicpitch/model.ort.c)
target_include_directories(basicpitch PUBLIC third_party/basicpitch third_party/eigen third_party/libremidi/include ${ORT_DIR}/include)
target_compile_definitions(basicpitch PUBLIC LIBREMIDI_HEADER_ONLY=1 NDEBUG EIGEN_MPL2_ONLY)
set_target_properties(basicpitch PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(basicpitch PRIVATE /O2 /bigobj /wd4244 /wd4267)
else()
    target_compile_options(basicpitch PRIVATE -O3 -w)
endif()
target_link_libraries(basicpitch PUBLIC ${ORT_LIB})

# ---- the plugin ----
if(APPLE)
    set(XT_FORMATS VST3 AU Standalone)
else()
    set(XT_FORMATS VST3 Standalone)
endif()

juce_add_plugin(XTRACT
    COMPANY_NAME "DennyAudio"
    PLUGIN_MANUFACTURER_CODE Dnny
    PLUGIN_CODE Wxtr
    FORMATS ${XT_FORMATS}
    PRODUCT_NAME "WXRST XTRACT"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Tools
    AU_MAIN_TYPE kAudioUnitType_Effect)

if(MSVC)
    target_compile_options(XTRACT PUBLIC /utf-8 /bigobj)
endif()

target_sources(XTRACT PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Separator.cpp
    Source/Transcriber.cpp)

target_include_directories(XTRACT PRIVATE Source)

target_compile_definitions(XTRACT PUBLIC
    DENNY_VERSION="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(XTRACT PRIVATE
    demucs basicpitch
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- bundle the ONNX Runtime shared library next to each binary ----
if(APPLE)
    target_link_options(XTRACT PUBLIC "-Wl,-rpath,@loader_path/../Frameworks")
    foreach(fmt ${XT_FORMATS})
        set(tgt XTRACT_${fmt})
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:${tgt}>/Contents/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORT_RUNTIME_FILE}" "$<TARGET_BUNDLE_DIR:${tgt}>/Contents/Frameworks/"
            COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:${tgt}>"
            VERBATIM)
    endforeach()
    # install into the user's plugin folders (done here, after the dylib is in place)
    add_custom_command(TARGET XTRACT_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/WXRST XTRACT.vst3"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:XTRACT_VST3>" "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/WXRST XTRACT.vst3"
        VERBATIM)
    add_custom_command(TARGET XTRACT_AU POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$ENV{HOME}/Library/Audio/Plug-Ins/Components/WXRST XTRACT.component"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:XTRACT_AU>" "$ENV{HOME}/Library/Audio/Plug-Ins/Components/WXRST XTRACT.component"
        VERBATIM)
elseif(WIN32)
    # delay-load so we can LoadLibrary the DLL from the plugin's own folder (FL only searches its own dir)
    target_link_options(XTRACT PUBLIC "/DELAYLOAD:onnxruntime.dll")
    target_link_libraries(XTRACT PRIVATE delayimp)
    foreach(fmt ${XT_FORMATS})
        set(tgt XTRACT_${fmt})
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORT_RUNTIME_FILE}" "$<TARGET_FILE_DIR:${tgt}>/"
            VERBATIM)
    endforeach()
endif()

# ---- offline console test: separate + transcribe a file, no UI ----
option(XTRACT_BUILD_TEST "Build the console extractor test" OFF)
if(XTRACT_BUILD_TEST)
    juce_add_console_app(XTRACT_TEST PRODUCT_NAME "XtractTest")
    target_sources(XTRACT_TEST PRIVATE Source/XtractTest.cpp Source/Separator.cpp Source/Transcriber.cpp)
    target_include_directories(XTRACT_TEST PRIVATE Source)
    target_compile_definitions(XTRACT_TEST PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(XTRACT_TEST PRIVATE demucs basicpitch juce::juce_audio_formats juce::juce_dsp juce::juce_core juce::juce_graphics
        PUBLIC juce::juce_recommended_config_flags)
    if(APPLE)
        target_link_options(XTRACT_TEST PRIVATE "-Wl,-rpath,${ORT_DIR}/lib")
    endif()
endif()

if(XTRACT_BUILD_TEST)
    juce_add_console_app(NOTES_BENCH PRODUCT_NAME "NotesBench")
    target_sources(NOTES_BENCH PRIVATE Source/NotesBench.cpp Source/Separator.cpp Source/Transcriber.cpp)
    target_include_directories(NOTES_BENCH PRIVATE Source)
    target_compile_definitions(NOTES_BENCH PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(NOTES_BENCH PRIVATE demucs basicpitch juce::juce_audio_formats juce::juce_dsp juce::juce_core juce::juce_graphics
        PUBLIC juce::juce_recommended_config_flags)
    if(APPLE)
        target_link_options(NOTES_BENCH PRIVATE "-Wl,-rpath,${ORT_DIR}/lib")
    endif()
endif()
