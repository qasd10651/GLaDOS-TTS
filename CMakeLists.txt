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
    snt_voice.c
    snt_g2p.c
)

# ====== 處理資料複製與英文瘦身 ======
set(DATA_SRC ${CMAKE_BINARY_DIR}/espeak_ng-install/share/espeak-ng-data)
set(DATA_DST ${CMAKE_CURRENT_BINARY_DIR}/espeak-ng-data)


# ====== 雙軌編譯邏輯 ======
if(EMSCRIPTEN)
    if(EMSCRIPTEN)
    # WASM 模式：編譯橋接層
    add_executable(glados_tts ${CORE_SOURCES} wasm_bridge.c)
    
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -msimd128 -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -msimd128 -ffast-math")
    
    # 1. 優雅生成 Python 瘦身腳本 (採用明確的檔案白名單)
    set(CLEANUP_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/cleanup_dicts.py")
    file(WRITE ${CLEANUP_SCRIPT} "
import sys, os, glob

data_dir = sys.argv[1]

# 找出 espeak-ng-data 根目錄下所有的字典檔 (*_dict)
for path in glob.glob(os.path.join(data_dir, '*_dict')):
    name = os.path.basename(path)
    # 只要不是 en_dict (英文字典)，就通通刪除
    if name != 'en_dict':
        os.remove(path)
        print(f'已刪除不需要的字典: {name}')
    ")

    # 2. 確保在連結打包前執行複製與瘦身
    add_custom_command(TARGET glados_tts PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${DATA_SRC} ${DATA_DST}
        # 執行剛生成的 python 腳本，傳入 DATA_DST 路徑
        COMMAND python3 ${CLEANUP_SCRIPT} ${DATA_DST}
        COMMENT "正在清理非英文的 espeak-ng 字典檔..."
    )

    # 3. 加上 --preload-file 將瘦身後的字典打包成 .data，並匯出給 JS 的 API
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} \
        -s INITIAL_MEMORY=33554432 \
        -s ALLOW_MEMORY_GROWTH=1 \
        -s EXPORTED_RUNTIME_METHODS=ccall,cwrap,getValue,setValue,UTF8ToString,stringToUTF8,lengthBytesUTF8 \
        -s EXPORTED_FUNCTIONS=\"['_snt_g2p_init', '_api_get_next_sentence_ids', '_api_synthesize_chunk', '_malloc', '_free']\" \
        --preload-file ${DATA_DST}@/espeak-ng-data \
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

add_dependencies(glados_tts espeak_ng_external)

# ====== 連結設定 ======
target_include_directories(glados_tts PRIVATE ${ESPEAKNG_INSTALL_DIR}/include)
target_link_libraries(glados_tts PRIVATE ${ESPEAKNG_STATIC_LIB} ${UCD_STATIC_LIB} m)

if(WIN32 AND NOT EMSCRIPTEN)
    target_compile_definitions(glados_tts PRIVATE LIBESPEAK_NG_EXPORT)
endif()
