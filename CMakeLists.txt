cmake_minimum_required(VERSION 3.26)
project(glados_tts LANGUAGES C CXX)

include(ExternalProject)

# 設定 espeak-ng 的建置與安裝路徑
set(ESPEAKNG_BUILD_DIR ${CMAKE_BINARY_DIR}/espeak_ng)
set(ESPEAKNG_INSTALL_DIR ${CMAKE_BINARY_DIR}/espeak_ng-install)

# 根據作業系統設定靜態庫副檔名
if(MSVC)
    set(ESPEAKNG_STATIC_LIB ${ESPEAKNG_INSTALL_DIR}/lib/espeak-ng.lib)
    set(UCD_STATIC_LIB ${ESPEAKNG_BUILD_DIR}/src/espeak_ng_external-build/src/ucd-tools/ucd.lib)
else()
    set(ESPEAKNG_STATIC_LIB ${ESPEAKNG_INSTALL_DIR}/lib/libespeak-ng.a)
    set(UCD_STATIC_LIB ${ESPEAKNG_BUILD_DIR}/src/espeak_ng_external-build/src/ucd-tools/libucd.a)
endif()

# 自動下載並編譯 espeak-ng (與官方 Piper 相同的穩定版本)
ExternalProject_Add(espeak_ng_external
    GIT_REPOSITORY https://github.com/espeak-ng/espeak-ng.git
    GIT_TAG 724808c  
    PREFIX ${ESPEAKNG_BUILD_DIR}
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${ESPEAKNG_INSTALL_DIR}
        -DBUILD_SHARED_LIBS:BOOL=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
        -DUSE_ASYNC:BOOL=OFF
        -DUSE_MBROLA:BOOL=OFF
        -DUSE_LIBSONIC:BOOL=OFF
        -DUSE_LIBPCAUDIO:BOOL=OFF
        -DUSE_KLATT:BOOL=OFF
        -DUSE_SPEECHPLAYER:BOOL=OFF
        -DEXTRA_cmn:BOOL=ON
        -DEXTRA_ru:BOOL=ON
        "-DCMAKE_C_FLAGS=-D_FILE_OFFSET_BITS=64 -I${ESPEAKNG_BUILD_DIR}/src/espeak_ng_external/src/ucd-tools/src/include"
        "-DCMAKE_CXX_FLAGS=-D_FILE_OFFSET_BITS=64 -I${ESPEAKNG_BUILD_DIR}/src/espeak_ng_external/src/ucd-tools/src/include"
    BUILD_BYPRODUCTS
        ${ESPEAKNG_STATIC_LIB}
        ${UCD_STATIC_LIB}
    UPDATE_DISCONNECTED TRUE
)

if(EMSCRIPTEN)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -msimd128 -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -msimd128 -ffast-math")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -march=native -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -ffast-math")
endif()

# 定義我們的純 C 獨立執行檔
add_executable(glados_tts
    main.c
    snt_front_f32.c
    snt_piperlite.c
    snt_voice_wasm.c
    snt_g2p_wasm.c
)

# 確保先編譯 espeak-ng，再編譯我們的程式
add_dependencies(glados_tts espeak_ng_external)

# 包含 espeak-ng 的標頭檔路徑
target_include_directories(glados_tts PRIVATE
    ${ESPEAKNG_INSTALL_DIR}/include
)

# 連結 espeak-ng 靜態庫與標準數學庫 (m)
target_link_libraries(glados_tts PRIVATE
    ${ESPEAKNG_STATIC_LIB}
    ${UCD_STATIC_LIB}
    m
)

# 修復 Windows DLL thunk 錯誤 (解決 __imp_ 找不到的問題)
if(WIN32)
    target_compile_definitions(glados_tts PRIVATE LIBESPEAK_NG_EXPORT)
endif()

# 自動複製 espeak-ng-data 到執行檔旁邊
# set(DATA_SRC ${CMAKE_BINARY_DIR}/espeak_ng-install/share/espeak-ng-data)
# set(DATA_DST ${CMAKE_CURRENT_BINARY_DIR}/espeak-ng-data)

# add_custom_target(copy_espeak_ng_data ALL
#     COMMAND ${CMAKE_COMMAND} -E copy_directory ${DATA_SRC} ${DATA_DST}
#     DEPENDS espeak_ng_external glados_tts
#     COMMENT "正在複製 espeak-ng-data..."
# )

# 判斷是否為 Emscripten 編譯環境
if(EMSCRIPTEN)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} \
        -s INITIAL_MEMORY=33554432 \
        -s ALLOW_MEMORY_GROWTH=1 \
        -s EXPORTED_RUNTIME_METHODS=ccall,cwrap,getValue,setValue,UTF8ToString,stringToUTF8,lengthBytesUTF8 \
        -s EXPORT_ES6=1 \
        -s MODULARIZE=1 \
        -s EXPORT_NAME='GladosTTSModule'")
        
    # 將輸出副檔名改為 JS (Emscripten 會同時生成 .wasm 與 .js)
    set_target_properties(glados_tts PROPERTIES SUFFIX ".js")
endif()
