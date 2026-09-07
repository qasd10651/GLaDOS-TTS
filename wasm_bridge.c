// wasm_bridge.c
#include <stdint.h>
#include <emscripten/emscripten.h>

// 宣告您的核心純 C 函式
extern int snt_g2p_init(void);
extern int snt_g2p_text_to_ids(const char **text_ptr, int32_t *out_ids, int max_ids);
extern int snt_voice_synthesize(const uint8_t *front_blob, const uint8_t *dec_blob,
                                const int32_t *ids, int n_ids,
                                float length_scale, float *out, int out_cap);

// ============================================================================
// 👇 以下是專門開放給 JavaScript 呼叫的 API
// ============================================================================

// 1. 初始化引擎
EMSCRIPTEN_KEEPALIVE
int api_init_engine() {
    return snt_g2p_init();
}

// 2. 獲取下一個短句的音素 ID (把雙指標的複雜度留在 C 裡面，對 JS 更友善)
// JS 只需要傳入「儲存文字指標的記憶體位址」，C 會自動幫忙推進
EMSCRIPTEN_KEEPALIVE
int api_get_next_sentence_ids(uint32_t text_ptr_addr, int32_t *out_ids, int max_ids) {
    const char **text_ptr = (const char **)text_ptr_addr;
    if (!text_ptr || !*text_ptr) return 0;
    
    return snt_g2p_text_to_ids(text_ptr, out_ids, max_ids);
}

// 3. 執行推論產生音波
EMSCRIPTEN_KEEPALIVE
int api_synthesize_chunk(const uint8_t *front_blob, const uint8_t *dec_blob,
                         const int32_t *ids, int n_ids,
                         float *audio_out, int out_cap) {
    // 預設語速 1.0f
    return snt_voice_synthesize(front_blob, dec_blob, ids, n_ids, 1.0f, audio_out, out_cap);
}