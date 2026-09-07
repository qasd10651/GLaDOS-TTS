/* snt_g2p_wasm.c -- WebAssembly (Emscripten) espeak-ng G2P shim.
 *
 * Same phonemize contract as the ESP32-S3 port's esp_g2p.c: espeak-ng
 * (translator only, no audio synth) turns text into IPA, each codepoint maps
 * to a Piper phoneme id via a binary-searched per-voice CP_ID table, and the
 * BOS/pad/EOS framing matches piper's phonemes_to_ids exactly. Only
 * init differs -- the browser mounts espeak-ng-data through Emscripten's
 * preloaded virtual FS (--preload-file ...@/espeak) instead of SPIFFS, so
 * there is no esp_vfs_spiffs_register() call here.
 *
 * Multi-language: snt_g2p_set_voice(espeak_voice, voice_slot) switches both
 * the espeak voice (G2P rules/dict) and the id table (that voice's Piper
 * phoneme_id_map, see cp_id_tables_multi.h). Default stays en-us + kristin
 * (slot 0) so the original English path is unchanged.
 *
 * Do not change the id-mapping/framing logic without re-running the parity
 * gate (verify_g2p_node.mjs) -- the tables are the training-time contract.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "espeak_ng.h"
#include "speak_lib.h"

#include "cp_id_tables_multi.h"

#define CLAUSE_INTONATION_FULL_STOP 0x00000000
#define CLAUSE_INTONATION_COMMA 0x00001000
#define CLAUSE_INTONATION_QUESTION 0x00002000
#define CLAUSE_INTONATION_EXCLAMATION 0x00003000
#define CLAUSE_TYPE_CLAUSE 0x00040000
#define CLAUSE_TYPE_SENTENCE 0x00080000

#define CLAUSE_PERIOD (40 | CLAUSE_INTONATION_FULL_STOP | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_COMMA (20 | CLAUSE_INTONATION_COMMA | CLAUSE_TYPE_CLAUSE)
#define CLAUSE_QUESTION (40 | CLAUSE_INTONATION_QUESTION | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_EXCLAMATION (45 | CLAUSE_INTONATION_EXCLAMATION | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_COLON (30 | CLAUSE_INTONATION_FULL_STOP | CLAUSE_TYPE_CLAUSE)
#define CLAUSE_SEMICOLON (30 | CLAUSE_INTONATION_COMMA | CLAUSE_TYPE_CLAUSE)

#define ID_BOS 1
#define ID_PAD 0
#define ID_EOS 2

/* phonememode for snt_g2p_text_to_ipa() -- byte-for-byte the value python
 * phonemizer 3.x passes to espeak_TextToPhonemes() when EspeakBackend is
 * constructed with tie=... (see phonemizer/backend/espeak/wrapper.py:353,
 * "phonemes_mode = 0x02 | 0x01 << 7 | ord('͡') << 8").
 *
 * Justified by mcu/ports/wasm/espeak/include/espeak-ng/speak_lib.h:
 *   #define espeakPHONEMES_IPA  0x02   -- "bit 1: ... 1= International
 *                                          Phonetic Alphabet (as UTF-8)"
 *   #define espeakPHONEMES_TIE  0x80   -- "bit 7: use (bits 8-23) as a tie
 *                                          within multi-letter phoneme names"
 *   bits 8-23                          -- the tie/separator character itself
 *
 * phonemizer always asks espeak for the DEFAULT U+0361 COMBINING DOUBLE
 * INVERTED BREVE and only afterwards rewrites it to the caller's tie
 * character ('^' for misaki), so this shim emits U+0361 too and leaves the
 * '͡' -> '^' rewrite to the JS side (web/trellis_frontend.js).
 *
 *   0x02 | 0x80 | (0x361 << 8) == 0x36182 == 221570
 */
#define SNT_TIE_CP 0x0361
#define SNT_IPA_TIE_PHONEMEMODE (espeakPHONEMES_IPA | espeakPHONEMES_TIE | (SNT_TIE_CP << 8))

/* Name of the espeak voice currently selected, so snt_g2p_text_to_ipa() can
 * force "en-us" and put the caller's voice back -- the legacy
 * snt_g2p_text_to_ids() path must not observe any state change. */
static char g_voice_name[64] = "en-us";

/* Active id table -- defaults to kristin (slot 0), the original contract. */
static const cp_id_t *g_tab = CP_ID_KRISTIN;
static int g_tab_n = (int)(sizeof(CP_ID_KRISTIN) / sizeof(CP_ID_KRISTIN[0]));

static int lookup(unsigned cp) {
    int lo = 0, hi = g_tab_n;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_tab[m].cp < cp) lo = m + 1; else hi = m; }
    return (lo < g_tab_n && g_tab[lo].cp == cp) ? g_tab[lo].id : -1;
}

static const char *nextcp(const char *s, unsigned *cp) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) { *cp = c; return s + 1; }
    if ((c >> 5) == 6) { *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); return s + 2; }
    if ((c >> 4) == 14) { *cp = ((c & 0xF) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return s + 3; }
    if ((c >> 3) == 30) { *cp = ((c & 7) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); return s + 4; }
    *cp = c; return s + 1;
}

static int g_ready = 0;

/* Idempotent: safe to call once from JS before the first phonemize call, or
 * to leave to the lazy init inside snt_g2p_text_to_ids(). */
int snt_g2p_init(void) {
    if (g_ready) return 0;

    espeak_ng_InitializePath("./espeak-ng-data");
    espeak_ng_ERROR_CONTEXT ctx = NULL;
    espeak_ng_STATUS s = espeak_ng_Initialize(&ctx);
    if (s != ENS_OK) { printf("g2p: espeak init failed (0x%x)\n", (unsigned)s); return -2; }

    s = espeak_ng_SetVoiceByName("en-us");
    if (s != ENS_OK) {
        /* en-us not resolved for some reason -- fall back to the base "en"
         * voice the ESP32 port uses; still English G2P, same rule tables. */
        s = espeak_ng_SetVoiceByName("en");
        if (s != ENS_OK) { printf("g2p: set voice failed (0x%x)\n", (unsigned)s); return -3; }
        snprintf(g_voice_name, sizeof(g_voice_name), "%s", "en");
    } else {
        snprintf(g_voice_name, sizeof(g_voice_name), "%s", "en-us");
    }
    g_ready = 1;
    printf("g2p: espeak ready (voice en-us)\n");
    return 0;
}

/* Select the espeak voice (G2P rules + dictionary) and the Piper id table
 * for one of the release voices. voice_slot indexes SNT_VOICE_TABS (see
 * cp_id_tables_multi.h for the slot order); espeak_voice is the espeak
 * voice name from that voice's Piper config ("en-us", "vi", "cmn", ...).
 * Passing them separately lets e.g. kristin (table slot 0) run under either
 * "en" (its config voice, exact Piper parity) or "en-us" (the historical
 * default). Returns 0 on success. */
int snt_g2p_set_voice(const char *espeak_voice, int voice_slot) {
    if (!g_ready) { int rc = snt_g2p_init(); if (rc != 0) return -1; }
    if (!espeak_voice || voice_slot < 0 || voice_slot >= SNT_NUM_VOICE_TABS) return -1;

    espeak_ng_STATUS s = espeak_ng_SetVoiceByName(espeak_voice);
    if (s != ENS_OK) {
        printf("g2p: set voice '%s' failed (0x%x)\n", espeak_voice, (unsigned)s);
        return -2;
    }
    snprintf(g_voice_name, sizeof(g_voice_name), "%s", espeak_voice);
    g_tab = SNT_VOICE_TABS[voice_slot].tab;
    g_tab_n = SNT_VOICE_TABS[voice_slot].n;
    printf("g2p: voice '%s' + id table '%s' (slot %d, %d entries)\n",
           espeak_voice, SNT_VOICE_TABS[voice_slot].name, voice_slot, g_tab_n);
    return 0;
}

/* Raw IPA phoneme string for the Trellis-RIFT (Misaki/Kokoro) frontend.
 *
 * This is NOT the legacy Piper path: no id mapping, no BOS/PAD/EOS framing,
 * nothing about snt_g2p_text_to_ids() changes. It reproduces
 * phonemizer.backend.espeak.wrapper.EspeakWrapper.text_to_phonemes(text,
 * tie='͡') exactly: repeatedly call espeak_TextToPhonemes() until the text
 * pointer becomes NULL, drop empty clause results, and join the remaining
 * clause strings with a single space.
 *
 * Output carries stress marks (ˈ ˌ), IPA symbols and U+0361 ties. The
 * misaki-side '͡' -> '^' rewrite, punctuation restoration and E2M
 * normalization all happen in web/trellis_frontend.js.
 *
 * The espeak voice is forced to "en-us" (misaki EspeakFallback(british=False))
 * and the caller's previous voice is restored before returning.
 *
 * Returns the number of bytes written (excluding the NUL terminator), or a
 * negative error code:
 *   -1 bad arguments / init failure
 *   -2 could not select the "en-us" voice
 *   -3 output buffer too small (out is left NUL-terminated but truncated)
 *   -4 clause loop did not terminate (espeak refused to advance the pointer)
 *   -5 could not restore the caller's voice (output is still valid)
 */
int snt_g2p_text_to_ipa(const char *text, char *out, int max_len) {
    if (!g_ready) { int rc = snt_g2p_init(); if (rc != 0) return -1; }
    if (!text || !out || max_len <= 0) return -1;
    out[0] = '\0';

    char prev_voice[sizeof(g_voice_name)];
    snprintf(prev_voice, sizeof(prev_voice), "%s", g_voice_name);
    int switched = strcmp(prev_voice, "en-us") != 0;
    if (switched && espeak_ng_SetVoiceByName("en-us") != ENS_OK) {
        printf("g2p: text_to_ipa could not select en-us\n");
        return -2;
    }

    int n = 0;          /* bytes written so far, excluding NUL */
    int clauses = 0;    /* number of non-empty clause results appended */
    int rc = 0;
    const void *tp = text;
    int guard = 0;

    /* python: `while text_ptr.contents.value is not None` -- the loop ends
     * when espeak nulls the pointer, NOT when it reaches an empty string. */
    while (tp != NULL) {
        if (guard++ > 4096) { rc = -4; break; }
        const char *ph = espeak_TextToPhonemes(&tp, espeakCHARS_UTF8, SNT_IPA_TIE_PHONEMEMODE);
        if (!ph || !*ph) continue;   /* python skips falsy (empty) results */
        int need = (int)strlen(ph) + (clauses > 0 ? 1 : 0);
        if (n + need + 1 > max_len) { rc = -3; break; }
        if (clauses > 0) out[n++] = ' ';
        memcpy(out + n, ph, strlen(ph));
        n += (int)strlen(ph);
        out[n] = '\0';
        clauses++;
    }

    if (switched && espeak_ng_SetVoiceByName(prev_voice) != ENS_OK) {
        printf("g2p: text_to_ipa could not restore voice '%s'\n", prev_voice);
        if (rc == 0) rc = -5;
    }
    return rc != 0 ? rc : n;
}

int snt_g2p_text_to_ids(const char **text_ptr, int32_t *out_ids, int max_ids) {
    if (!g_ready) { int rc = snt_g2p_init(); if (rc != 0) return -1; }
    if (!text_ptr || !*text_ptr || !out_ids || max_ids <= 0) return -1;

    int n = 0;
    if (n + 2 > max_ids) return -1;
    out_ids[n++] = ID_BOS; out_ids[n++] = ID_PAD;

    const void *tp = *text_ptr;
    int guard = 0;
    int has_phonemes = 0;
    
    while (tp != NULL && guard++ < 256) {
        int terminator = 0;
        // espeak 會自動解析到下一個停頓點
        const char *ph = espeak_TextToPhonemesWithTerminator(
            &tp, espeakCHARS_UTF8, espeakPHONEMES_IPA, &terminator);

        if (ph) {
            const char *p = ph; unsigned cp; int paren = 0;
            while (*p) {
                p = nextcp(p, &cp);
                if (cp == '(') { paren = 1; continue; }
                if (cp == ')') { paren = 0; continue; }
                if (paren) continue;
                int id = lookup(cp);
                if (id >= 0 && n + 2 <= max_ids) { 
                    out_ids[n++] = id; out_ids[n++] = ID_PAD; 
                    has_phonemes = 1;
                }
            }
        }

        terminator &= 0x000FFFFF;
        char tc = 0;
        if (terminator == CLAUSE_PERIOD) tc = '.';
        else if (terminator == CLAUSE_QUESTION) tc = '?';
        else if (terminator == CLAUSE_EXCLAMATION) tc = '!';
        else if (terminator == CLAUSE_COMMA) tc = ',';
        else if (terminator == CLAUSE_COLON) tc = ':';
        else if (terminator == CLAUSE_SEMICOLON) tc = ';';

        if (tc) {
            int id = lookup((unsigned)tc);
            if (id >= 0 && n + 2 <= max_ids) { 
                out_ids[n++] = id; out_ids[n++] = ID_PAD; 
            }
            if (tc == ',' || tc == ':' || tc == ';') { 
                int sid = lookup(' '); 
                if (sid >= 0 && n + 2 <= max_ids) { out_ids[n++] = sid; out_ids[n++] = ID_PAD; } 
            }
        }
        
        // 核心修改：一旦遇到 "句子級別" 的停頓點 (句號、問號、驚嘆號)，且已經有音素，就斷開這回合
        if (has_phonemes && (terminator & CLAUSE_TYPE_SENTENCE || tp == NULL)) {
            break;
        }
    }

    if (n + 1 <= max_ids) out_ids[n++] = ID_EOS;
    
    *text_ptr = (const char *)tp; // 更新外部文字指標，下次從這繼續
    
    // 如果這輪連一個實質的音素都沒找到(例如都是空白)，回傳 0 讓外部自然結束
    return has_phonemes ? n : 0;
}
