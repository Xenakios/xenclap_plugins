cmake_minimum_required(VERSION 3.15)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include_directories(
    libs/clap/include
    libs/clap-helpers/include
    source/noiseplethora/noise-plethora/rack/include
    libs/signalsmith-stretch
    libs/choc
    libs/sst-basic-blocks/include
    libs/fmt/include
    libs/sst-voicemanager/include
    libs/tuning-library/include
    libs/MTS-ESP/Client
    source/noiseplethora/noise-plethora/rack/include/simd
)

project(xenclapplugins)

add_subdirectory(libs/fmt)
# set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "Get em")
# add_subdirectory(libs/clap-wrapper)

# set(VST3_TARGET noise_plethora_vst3)
# add_library(${VST3_TARGET} MODULE)
# target_sources(${VST3_TARGET} PRIVATE source/noiseplethorasynth.cpp)
# target_compile_definitions(noise_plethora_vst3 PRIVATE _USE_MATH_DEFINES=1)

add_library(FilePlayerPlugin MODULE
source/fileplayer/fileplayer.cpp
)

set_target_properties(FilePlayerPlugin PROPERTIES SUFFIX ".clap" PREFIX "")
target_compile_definitions(FilePlayerPlugin PRIVATE _USE_MATH_DEFINES=1 IS_WIN=1)
target_link_libraries(FilePlayerPlugin PRIVATE fmt)
set(products_folder ${CMAKE_BINARY_DIR})
    set(build_type ${CMAKE_BUILD_TYPE})
    add_custom_command(
        TARGET FilePlayerPlugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${products_folder}/FilePlayerPlugin.clap" "C:\\Program Files\\Common Files\\CLAP"
    )

add_library(NoisePlethoraSynth MODULE
source/noiseplethora/noiseplethorasynth.cpp
)

set_target_properties(NoisePlethoraSynth PROPERTIES SUFFIX ".clap" PREFIX "")
target_compile_definitions(NoisePlethoraSynth PRIVATE _USE_MATH_DEFINES=1 IS_WIN=1)
target_link_libraries(NoisePlethoraSynth PRIVATE noiseplethora fmt)
set(products_folder ${CMAKE_BINARY_DIR})
    set(build_type ${CMAKE_BUILD_TYPE})
    add_custom_command(
        TARGET NoisePlethoraSynth POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${products_folder}/NoisePlethoraSynth.clap" "C:\\Program Files\\Common Files\\CLAP"
    )



add_library(noiseplethora
    source/noiseplethora/noise-plethora/random.cpp
    source/noiseplethora/noise-plethora/plugins/Banks.cpp
    
)
file(GLOB_RECURSE NPSOURCES ${CMAKE_CURRENT_SOURCE_DIR}/source/noiseplethora/noise-plethora/teensy/*.cpp)
target_sources(noiseplethora PRIVATE ${NPSOURCES})
target_compile_definitions(noiseplethora PRIVATE _USE_MATH_DEFINES=1)

add_library(KlangAS MODULE
source/klangas/klangassynth.cpp
source/klangas/sineosc.cpp
)

set_target_properties(KlangAS PROPERTIES SUFFIX ".clap" PREFIX "")
target_compile_definitions(KlangAS PRIVATE _USE_MATH_DEFINES=1 IS_WIN=1)
target_link_libraries(KlangAS PRIVATE fmt)
set(products_folder ${CMAKE_BINARY_DIR})
    set(build_type ${CMAKE_BUILD_TYPE})
    add_custom_command(
        TARGET KlangAS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${products_folder}/KlangAS.clap" "C:\\Program Files\\Common Files\\CLAP"
    )

add_executable(TestingProgram 
source/klangas/sineosc.cpp
source/main.cpp
)
target_link_libraries(TestingProgram PRIVATE noiseplethora fmt)
target_compile_definitions(TestingProgram PRIVATE NOJUCE=1 _USE_MATH_DEFINES=1 __WINDOWS_WASAPI__)

add_library(GeneratedPlugin MODULE
libs/MTS-ESP/Client/libMTSClient.cpp
python/clapgen/gritnoise.cpp

)

set_target_properties(GeneratedPlugin PROPERTIES SUFFIX ".clap" PREFIX "")
target_compile_definitions(GeneratedPlugin PRIVATE _USE_MATH_DEFINES=1 IS_WIN=1)
target_link_libraries(GeneratedPlugin PRIVATE fmt)
set(products_folder ${CMAKE_BINARY_DIR})
set(build_type ${CMAKE_BUILD_TYPE})
add_custom_command(
        TARGET GeneratedPlugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${products_folder}/GeneratedPlugin.clap" "C:\\Program Files\\Common Files\\CLAP"
)
