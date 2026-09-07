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

# ====== 您的神級修改：強制傳遞 WASM 工具鏈給 espeak-ng ======
set(EXTRA_EXTERNAL_CMAKE_ARGS "")
if(EMSCRIPTEN)
    list(APPEND EXTRA_EXTERNAL_CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_C_COMPILER=emcc
        -DCMAKE_CXX_COMPILER=em++
        -DCMAKE_AR=emar
        -DCMAKE_RANLIB=emranlib
        "-DCMAKE_C_FLAGS=-D_FILE_OFFSET_BITS=64 -I${ESPEAKNG_BUILD_DIR}/src/espeak_ng_external/src/ucd-tools/src/include -O3 -msimd128"
        "-DCMAKE_CXX_FLAGS=-D_FILE_OFFSET_BITS=64 -I${ESPEAKNG_BUILD_DIR}/src/espeak_ng_external/src/ucd-tools/src/include -O3 -msimd128"
    )
else()
    list(APPEND EXTRA_EXTERNAL_CMAKE_ARGS
        "-DCMAKE_C_FLAGS=-D_FILE_OFFSET_BITS=64 -I${ESPEAKNG_BUILD_DIR}/src/espeak_ng_external/src/ucd-tools/src/include"
        "-DCMAKE_CXX_FLAGS=-D_FILE_OFFSET_BITS=64 -I${ESPEAKNG_BUILD_DIR}/src/espeak_ng_external/src/ucd-tools/src/include"
    )
endif()

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
        ${EXTRA_EXTERNAL_CMAKE_ARGS}
    BUILD_BYPRODUCTS
        ${ESPEAKNG_STATIC_LIB}
        ${UCD_STATIC_LIB}
    UPDATE_DISCONNECTED TRUE
)

# ====== 核心共用檔案 ======
set(CORE_SOURCES
    snt_front_f32.c
    snt_piperlite.c
    snt_voice_wasm.c
    snt_g2p_wasm.c
)

# ====== 處理資料複製與英文瘦身 ======
set(DATA_SRC ${CMAKE_BINARY_DIR}/espeak_ng-install/share/espeak-ng-data)
set(DATA_DST ${CMAKE_CURRENT_BINARY_DIR}/espeak-ng-data)

# 自動複製字典，並在複製後刪除 voices 目錄下除了 en 之外的所有語言，達成極致瘦身
add_custom_target(prepare_espeak_data ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${DATA_SRC} ${DATA_DST}
    # 建立一個臨時的 python 腳本來安全刪除不必要的語言檔案
    COMMAND ${CMAKE_COMMAND} -E echo "import os, shutil, glob; [shutil.rmtree(f) if os.path.isdir(f) else os.remove(f) for f in glob.glob('${DATA_DST}/voices/*') if not os.path.basename(f).startswith('en') and not os.path.basename(f) == '!v']" > cleanup_voices.py
    COMMAND python cleanup_voices.py
    DEPENDS espeak_ng_external
    COMMENT "正在準備並精簡 espeak-ng-data (僅保留英文)..."
)


# ====== 雙軌編譯邏輯 ======
if(EMSCRIPTEN)
    # WASM 模式：編譯橋接層
    add_executable(glados_tts ${CORE_SOURCES} wasm_bridge.c)
    
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -msimd128 -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -msimd128 -ffast-math")
    
    # 加上 --preload-file 將瘦身後的字典打包成 .data
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} \
        -s INITIAL_MEMORY=33554432 \
        -s ALLOW_MEMORY_GROWTH=1 \
        -s EXPORTED_RUNTIME_METHODS=ccall,cwrap,getValue,setValue,UTF8ToString,stringToUTF8,lengthBytesUTF8 \
        -s EXPORT_ES6=1 \
        -s MODULARIZE=1 \
        -s EXPORT_NAME='GladosTTSModule'")
        
    set_target_properties(glados_tts PROPERTIES SUFFIX ".js")
else()
    # 桌面模式：編譯主程式
    add_executable(glados_tts ${CORE_SOURCES} main.c)
    
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -march=native -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -ffast-math")
endif()

add_dependencies(glados_tts prepare_espeak_data)

# ====== 連結設定 ======
target_include_directories(glados_tts PRIVATE ${ESPEAKNG_INSTALL_DIR}/include)
target_link_libraries(glados_tts PRIVATE ${ESPEAKNG_STATIC_LIB} ${UCD_STATIC_LIB} m)

if(WIN32 AND NOT EMSCRIPTEN)
    target_compile_definitions(glados_tts PRIVATE LIBESPEAK_NG_EXPORT)
endif()