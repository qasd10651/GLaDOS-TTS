#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

// --- 宣告將要連結的 C 引擎函式 ---
int snt_voice_synthesize(const uint8_t *front_blob, const uint8_t *dec_blob,
                         const int32_t *ids, int n_ids,
                         float length_scale,
                         float *out, int out_cap);

// 宣告 G2P 文字轉音素函式 (來自 snt_g2p_wasm.c)
int snt_g2p_init(void);
int snt_g2p_text_to_ids(const char **text_ptr, int32_t *out_ids, int max_ids);

// --- 輔助函式：讀取檔案 ---
uint8_t* read_file(const char* filename, size_t* out_size) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        printf("找不到檔案: %s\n", filename);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc(size);
    fread(buf, 1, size, f);
    fclose(f);
    *out_size = size;
    return buf;
}

// --- 輔助函式：儲存 WAV ---
void save_wav(const char *filename, const float *audio, int num_samples, int sample_rate) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;
    int num_channels = 1;      
    int bits_per_sample = 16;  
    int byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
    int block_align = num_channels * (bits_per_sample / 8);
    int data_chunk_size = num_samples * block_align;
    int file_size = 36 + data_chunk_size;

    fwrite("RIFF", 1, 4, f); fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f); fwrite("fmt ", 1, 4, f);
    int fmt_chunk_size = 16; fwrite(&fmt_chunk_size, 4, 1, f);
    short audio_format = 1;  fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f); fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_chunk_size, 4, 1, f);

    for (int i = 0; i < num_samples; i++) {
        float v = audio[i];
        if (v < -1.0f) v = -1.0f; 
        if (v > 1.0f) v = 1.0f;
        short pcm_val = (short)rintf(v * 32767.0f); 
        fwrite(&pcm_val, 2, 1, f);
    }
    fclose(f);
    printf("成功儲存音檔: %s\n", filename);
}

// --- 主程式 ---
int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("用法: %s \"請輸入要讓 GLaDOS 說的英文句子\"\n", argv[0]);
        return 1;
    }
    char *input_text = argv[1];

    printf("啟動 GLaDOS 純 C 語言推論引擎...\n");

    if (snt_g2p_init() != 0) {
        printf("G2P 初始化失敗！\n");
        return 1;
    }

    size_t front_size, dec_size;
    uint8_t *front_blob = read_file("front_bundle.bin", &front_size);
    uint8_t *dec_blob = read_file("glados_bundle.bin", &dec_size);
    if (!front_blob || !dec_blob) return 1;

    // 準備輸出記憶體，這裡稍微開大一點到 60 秒 (22050 * 60 = 1,323,000 floats)
    int sample_rate = 22050;
    int max_samples = sample_rate * 60;
    float *audio_out = malloc(max_samples * sizeof(float));
    int total_samples = 0;

    // 設定句間靜音: 0.12 秒 (與 Python 原版設定一致)
    int silence_samples = (int)(sample_rate * 0.12f);

    const char *current_text = input_text;
    printf("開始神經網路推論...\n");

    // 迴圈：只要還有文字沒處理完，就繼續
    while (current_text != NULL) {
        int32_t text_ids[2048];
        
        // 傳入 &current_text，函式內部會自動推進這個指標
        int n_ids = snt_g2p_text_to_ids(&current_text, text_ids, 2048);
        if (n_ids <= 0) break; // 已經沒有更多語音需要合成了

        // 若不是第一句話，先填入 0.12 秒的靜音
        if (total_samples > 0) {
            if (total_samples + silence_samples < max_samples) {
                for (int i = 0; i < silence_samples; i++) {
                    audio_out[total_samples + i] = 0.0f;
                }
                total_samples += silence_samples;
            }
        }

        // 計算剩餘可以裝音訊的 Buffer 大小
        int chunk_cap = max_samples - total_samples;
        if (chunk_cap <= 0) {
            printf("警告：音訊太長，已達到 Buffer 限制！\n");
            break;
        }
        
        // 將推論結果接在已有的 audio_out 後面 (audio_out + total_samples)
        int chunk_samples = snt_voice_synthesize(
            front_blob, dec_blob, text_ids, n_ids, 
            1.0f, audio_out + total_samples, chunk_cap
        );

        if (chunk_samples < 0) {
            printf("句子推論失敗！錯誤碼: %d\n", chunk_samples);
            break;
        }
        
        total_samples += chunk_samples;
    }

    printf("推論完成！共生成 %d 個樣本。\n", total_samples);
    save_wav("glados_output.wav", audio_out, total_samples, sample_rate);

    free(front_blob); free(dec_blob); free(audio_out);
    return 0;
}