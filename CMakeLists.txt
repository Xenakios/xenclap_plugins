cmake_minimum_required(VERSION 3.15)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(
    libs/clap/include
    libs/clap-helpers/include
    source/noiseplethora/noise-plethora/rack/include
    libs/signalsmith-stretch
    libs/choc
    libs/sst-basic-blocks/include
    libs/fmt/include
    libs/sst-voicemanager/include
)

project(xenclapplugins)

add_subdirectory(libs/fmt)
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "Get em")
add_subdirectory(libs/clap-wrapper)

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
    # source/noise-plethora/teensy/effect_freeverb.cpp
    # source/noise-plethora/teensy/effect_bitcrusher.cpp
    # source/noise-plethora/teensy/effect_granular.cpp
    # source/noise-plethora/teensy/effect_combine.cpp
    # source/noise-plethora/teensy/effect_multiply.cpp
    # source/noise-plethora/teensy/effect_flange.cpp
    # source/noise-plethora/teensy/effect_wavefolder.cpp
    # source/noise-plethora/teensy/filter_variable.cpp
    # source/noise-plethora/teensy/synth_pwm.cpp
    # source/noise-plethora/teensy/synth_pinknoise.cpp
    # source/noise-plethora/teensy/synth_whitenoise.cpp
    source/noiseplethora/noise-plethora/plugins/Banks.cpp
    
)
file(GLOB_RECURSE NPSOURCES ${CMAKE_CURRENT_SOURCE_DIR}/source/noiseplethora/noise-plethora/teensy/*.cpp)
target_sources(noiseplethora PRIVATE ${NPSOURCES})
target_compile_definitions(noiseplethora PRIVATE _USE_MATH_DEFINES=1)
