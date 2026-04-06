/**
 * @file    config.h
 * @brief   TACTICAL TIMER 2 — 全域常數、列舉、資料結構定義
 *
 * 設計原則：
 *  - 單一真理來源 (Single Source of Truth)：所有常數、型別、結構定義
 *    集中於此，其他模組只 #include "config.h"，不得散落定義。
 *  - 純宣告：不含任何實作邏輯、全域變數實例、HAL 呼叫。
 *  - 向前相容：新增常數在對應分區末尾追加，不得插入已存在項目之間。
 *
 * 硬體目標：M5Stack Core2 (ESP32-D0WDQ6-V3, 雙核 240MHz)
 * IDE：Arduino IDE 1.8.x / ESP32 Arduino Core 2.x (IDF 4.4.x)
 *
 * @version 2.1.0
 * @date    2025-03
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <driver/i2s.h>          // ← I2S_NUM_0, i2s_mode_t 等型別定義

// ============================================================
// [C1] I2S 版本相容巨集
//      I2S_COMM_FORMAT_PDM 僅存在於 IDF 5.x+
//      IDF 4.4.x (Arduino Core 2.x) 使用 I2S_COMM_FORMAT_I2S
// ============================================================
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  #define TT2_MIC_COMM_FMT   I2S_COMM_FORMAT_STAND_I2S
#else
  #define TT2_MIC_COMM_FMT   I2S_COMM_FORMAT_I2S
#endif
/// M5Core2 SPM1423 LR 腳接 GND → 使用右聲道
#define TT2_MIC_CH_FMT     I2S_CHANNEL_FMT_ONLY_RIGHT


// ============================================================
// [C2] 版本資訊
// 所有成員名稱加 TT2_ 前綴，確保不與任何系統巨集衝突
// ============================================================
namespace TT2Version {
  constexpr char    TT2_APP_NAME[]  = "TACTICAL TIMER 2";
  constexpr char    TT2_APP_VER[]   = "2.1.0";
  constexpr char    TT2_AUTHOR[]    = "Louis Chou";
  constexpr uint8_t TT2_MAJOR       = 2;
  constexpr uint8_t TT2_MINOR       = 0;
  constexpr uint8_t TT2_PATCH       = 0;
}


// ============================================================
// [C3] 硬體 Pin / 硬體常數
// ============================================================
namespace HW {
  // --- I2S Audio 輸出 ---
  constexpr uint8_t  I2S_BCLK     = 12;
  constexpr uint8_t  I2S_LRCK     = 0;
  constexpr uint8_t  I2S_DOUT     = 2;
  constexpr float    OUTPUT_GAIN  = 0.4f;  ///< AudioOutputI2S gain（0.0–4.0，降低可減少削波破音）

  // --- SPM1423 PDM 麥克風 ---
  constexpr int      MIC_PORT     = I2S_NUM_0;  ///< Audio 與 Mic 共用，時分複用
  constexpr uint8_t  MIC_CLK_PIN  = 0;          ///< PDM CLK
  constexpr uint8_t  MIC_DATA_PIN = 34;         ///< PDM DATA (LR=GND → 右聲道)
  constexpr int      MIC_RATE     = 16000;       ///< 採樣率 Hz
  constexpr int      MIC_FRAME    = 256;         ///< 每幀樣本數（≈16ms @16kHz）

  // --- LCD ---
  constexpr int16_t  LCD_W        = 320;
  constexpr int16_t  LCD_H        = 240;
}


// ============================================================
// [C4] 音效檔路徑（SD 卡）
// 所有成員名稱加 TT2_ 前綴，確保不與任何系統巨集衝突
// ============================================================
namespace SND {
  constexpr char TT2_READY[]    = "/mp3/7001.mp3";  ///< "Are You Ready"
  constexpr char TT2_BEEP[]     = "/mp3/7002.mp3";  ///< 開始嗶聲
  constexpr char TT2_CLEAR[]    = "/mp3/7003.mp3";  ///< "Show Clear"
  constexpr char TT2_DRY_TICK[] = "/mp3/tick.mp3";  ///< Dry Fire 節拍音
}


// ============================================================
// [C5] 遊戲 / 計時常數
// ============================================================
namespace Timing {
  // --- Random delay 範圍（ms）---
  /// 最短備妥延遲（固定模式下使用固定值）
  constexpr uint32_t READY_DELAY_FIXED   = 2000;
  /// 隨機模式：最短 / 最長延遲
  constexpr uint32_t READY_DELAY_MIN     = 1000;
  constexpr uint32_t READY_DELAY_MAX     = 4000;

  /// SHOWCLEAR 延遲後播放 clear 音效
  constexpr uint32_t AFTER_CLEAR_DELAY   = 1000;

  /// 嗶聲結束後，保護窗口（避免喇叭尾音誤觸發麥克風）
  constexpr uint32_t GOING_GUARD_MS      = 600;

  /// Status bar / IP bar 更新週期
  constexpr uint32_t INFO_INTERVAL_MS    = 2000;

  /// Dry Fire 預設節拍間隔（ms）
  constexpr uint32_t DRY_BEAT_DEF_MS     = 2000;
  constexpr uint32_t DRY_BEAT_MIN_MS     = 500;
  constexpr uint32_t DRY_BEAT_MAX_MS     = 5000;

  /// Par Time 預設（0 = 停用）
  constexpr uint32_t PAR_TIME_DEF_MS     = 0;
  constexpr uint32_t PAR_TIME_MAX_MS     = 30000;
  constexpr uint32_t PAR_TIME_STEP_MS    = 100;
}


// ============================================================
// [C6] 麥克風偵測常數
// ============================================================
namespace MicCfg {
  constexpr int16_t  THRESH_DEF   = 2000;  ///< 預設 RMS 門檻（16-bit full scale = 32767）
  constexpr int16_t  THRESH_MIN   = 500;   ///< 最小可調值
  constexpr int16_t  THRESH_MAX   = 8000;  ///< 最大可調值
  constexpr int16_t  THRESH_STEP  = 200;   ///< 調整步進
  constexpr uint32_t COOLDOWN_MS  = 300;   ///< 擊發後冷卻時間（防止同槍二次觸發）
}


// ============================================================
// [C7] 資料容量限制
// ============================================================
namespace Limits {
  constexpr uint8_t  HIT_MAX       = 50;   ///< 每局最多擊發數（環形 buffer 上限）
  constexpr uint8_t  GAMES_MAX     = 20;   ///< RAM 中保留的最近局數（環型 buffer）
  constexpr uint8_t  STATION_MAX   = 6;    ///< ESP-NOW 最大遠端站台數
  constexpr uint8_t  PRESET_MAX    = 5;    ///< 最多儲存的 Preset 數量
  constexpr uint8_t  DRILL_MAX     = 10;   ///< 最多儲存的 Drill 定義數量
  constexpr uint16_t HISTORY_MAX   = 500;  ///< SD 卡歷史 session 最大筆數
  constexpr size_t   SD_PATH_LEN   = 48;   ///< SD 路徑最大長度
  constexpr size_t   NAME_LEN      = 16;   ///< 名稱字串最大長度（含 '\0'）
}


// ============================================================
// [C8] LCD 佈局常數
// ============================================================
namespace Layout {
  // --- 頂部狀態列 ---
  constexpr int16_t STATUS_H      = 20;    ///< 頂部狀態列高度
  constexpr int16_t GAME_AREA_Y   = STATUS_H;

  // --- 底部：IP 列 + 按鈕列 ---
  constexpr int16_t BTN_H         = 20;    ///< 實體按鍵對應視覺列高度
  constexpr int16_t IP_H          = 16;    ///< IP 位址顯示列高度
  constexpr int16_t BTN_Y         = HW::LCD_H - BTN_H;          ///< 220
  constexpr int16_t IP_Y          = BTN_Y - IP_H;               ///< 204
  constexpr int16_t GAME_AREA_H   = IP_Y - GAME_AREA_Y;         ///< 184

  // --- Home 6-card grid ---
  constexpr int16_t HOME_GRID_Y   = GAME_AREA_Y;
  constexpr int16_t HOME_GRID_H   = GAME_AREA_H;
  constexpr int16_t CARD_COLS     = 3;
  constexpr int16_t CARD_ROWS     = 2;
  constexpr int16_t CARD_GAP      = 3;
  constexpr int16_t CARD_PAD      = 4;

  // --- 模式畫面共用 ---
  constexpr int16_t MODE_HDR_H    = 18;    ///< 模式標題列高度
  constexpr int16_t HOME_BTN_W    = 60;    ///< 左下角 [HOME] 觸控區寬度
  constexpr int16_t HOME_BTN_H    = 20;    ///< 左下角 [HOME] 觸控區高度
  constexpr int16_t HOME_BTN_X    = 0;
  constexpr int16_t HOME_BTN_Y    = IP_Y - HOME_BTN_H;          ///< 184

  // --- Shot log 欄寬 ---
  constexpr int16_t COL2_X        = 175;   ///< 右欄起始 X
  constexpr int16_t SHOT_ROW_H    = 20;    ///< 每筆擊發列高度
}


// ============================================================
// [C9] RTOS 任務常數
// ============================================================
namespace TaskCfg {
  // Stack sizes (bytes) — 留有餘裕，ESP32 stack overflow 難除錯
  constexpr uint32_t  STACK_AUDIO  = 6144;
  constexpr uint32_t  STACK_MIC    = 8192;  ///< 增大：micBuf[512] + Serial.printf + WDT
  constexpr uint32_t  STACK_NET    = 6144;
  constexpr uint32_t  STACK_UI     = 8192;  ///< TT2 UI 畫面較複雜，加大

  // Priorities (higher = more urgent)
  constexpr UBaseType_t PRIO_AUDIO = 5;
  constexpr UBaseType_t PRIO_MIC   = 4;
  constexpr UBaseType_t PRIO_NET   = 3;
  constexpr UBaseType_t PRIO_UI    = 2;   ///< loopTask / UI Task

  // Core affinity
  constexpr BaseType_t  CORE_AUDIO = 0;
  constexpr BaseType_t  CORE_MIC   = 0;
  constexpr BaseType_t  CORE_NET   = 0;
  constexpr BaseType_t  CORE_UI    = 1;   ///< Arduino loop() 固定在 Core 1
}


// ============================================================
// [C10] Queue 深度
// ============================================================
namespace QDepth {
  constexpr uint8_t Q_AUDIO      = 4;
  constexpr uint8_t Q_AUDIO_DONE = 2;
  constexpr uint8_t Q_HIT        = 10;
  constexpr uint8_t Q_SSE        = 10;
  constexpr uint8_t Q_UI_CMD     = 8;
}


// ============================================================
// [C11] NVS 命名空間與鍵值
// ============================================================
namespace NVS {
  constexpr char NVS_NS[]        = "tt2_cfg";     ///< Namespace
  constexpr char HIT_SRC[]       = "hit_src";
  constexpr char MIC_THRESH[]    = "mic_thr";
  constexpr char ACTIVE_PRESET[] = "preset_idx";
  constexpr char RAND_DELAY[]    = "rand_dly";
  constexpr char PAR_TIME_MS[]   = "par_ms";
  constexpr char DRY_BEAT_MS[]   = "dry_beat";
  constexpr char MAX_SHOTS[]     = "max_shots";   ///< Free/RO 最大發數（0=無限）
  constexpr char DRILL_SHOTS[]   = "dr_shots";   ///< Drill 目標發數
  constexpr char DRILL_PAR_MS[]  = "dr_par";     ///< Drill Par Time（ms）
  constexpr char DRILL_PASS_PCT[]= "dr_pass";    ///< Drill Pass %
  constexpr char TZ_KEY[]        = "tz";
  constexpr char     FMT_VER[]     = "fmt_ver";
  // ★ 格式版本：Preset struct 改變時遞增，自動清除舊格式資料
  // v1 → v2：修正 Preset struct alignment 問題（HitSource padding）
  constexpr uint8_t  FMT_VER_VAL  = 2;
}


// ============================================================
// [C12] 網路 / NTP 常數
// ============================================================
namespace NetCfg {
  constexpr char     NTP_TZ[]   = "CST-8";             ///< 台灣 UTC+8 POSIX
  constexpr char     NTP_SRV1[] = "pool.ntp.org";
  constexpr char     NTP_SRV2[] = "time.cloudflare.com";
  constexpr uint8_t  NTP_TIMEOUT_S = 8;                ///< NTP 同步逾時（秒）
  constexpr uint16_t WEB_PORT   = 80;
  constexpr uint16_t ESPNOW_CHAN_ANY = 0;               ///< 0 = 使用 WiFi 目前 channel

  // AP 熱點設定（WiFi 連線失敗時自動啟用）
  constexpr char     AP_SSID[]     = "TacticalTimer2";  ///< 熱點名稱
  constexpr char     AP_PASSWORD[] = "tt2admin";        ///< 熱點密碼（至少 8 字元）
  constexpr char     AP_IP[]       = "192.168.4.1";     ///< AP 預設 IP
}


// ============================================================
// [C13] 列舉型別
// ============================================================

/** 應用程式模式（Home 六張卡片對應） */
enum class AppMode : uint8_t {
  NONE       = 0,
  HOME       = 1,   ///< Home 選單
  FREE       = 2,   ///< Free Shooting
  DRILL      = 3,   ///< Drills
  DRY_FIRE   = 4,   ///< Dry Fire
  SPY        = 5,   ///< Spy Mode
  RO         = 6,   ///< Range Officer Mode
  HISTORY    = 7,   ///< History browser
  SETTINGS   = 8,   ///< Settings
};

/** 遊戲執行狀態機（FSM）*/
enum class GameState : uint8_t {
  IDLE        = 0,  ///< 待機，等待 Start
  AREYOUREADY = 1,  ///< 播放 ready 音效中
  WAITING     = 2,  ///< 隨機/固定延遲倒數中
  BEEPING     = 3,  ///< 播放 start beep 中
  GOING       = 4,  ///< 計時中，接受命中
  SHOWCLEAR   = 5,  ///< 顯示成績，播放 clear 音效
  STOP        = 6,  ///< 已停止，顯示結果
};

/** 命中來源 */
enum class HitSource : uint8_t {
  ESP_NOW = 0,
  MIC     = 1,
  BOTH    = 2,   ///< TT2 新增：同時接受兩種來源
};

/** UI 介面模式 */
enum class UIMode : uint8_t {
  GAME    = 0,
  SETTING = 1,
};

/** ESP-NOW 封包類型 */
enum class MsgType : uint8_t {
  PAIRING = 0,
  DATA    = 1,
};

/** Drill 難度等級（供評分參考） */
enum class DrillLevel : uint8_t {
  BEGINNER     = 0,
  INTERMEDIATE = 1,
  ADVANCED     = 2,
  EXPERT       = 3,
};

/** Web → UI 指令 */
enum class WebCmd : uint8_t {
  NONE        = 0,
  START       = 1,
  STOP        = 2,
  RESTART     = 3,
  DRY_SET_BEAT= 4,
  SPY_CLEAR   = 5,  ///< SPY 清除紀錄
  SET_PAR     = 6,  ///< Web 設定 Par Time → Core2 同步
};


// ============================================================
// [C14] 資料結構
// ============================================================

// --- ESP-NOW 封包 ---
struct __attribute__((packed)) MsgData {
  uint8_t       msgType;    ///< MsgType::DATA
  uint8_t       id;         ///< Station ID (1–STATION_MAX)
  unsigned long hit_time;   ///< millis() 時間戳
};

struct MsgPairing {
  uint8_t msgType;          ///< MsgType::PAIRING
  uint8_t id;
  uint8_t macAddr[6];
  uint8_t channel;
};

// --- 擊發紀錄 ---
struct __attribute__((packed)) HitRecord {
  uint8_t       station_id;    ///< 0 = MIC, 1–STATION_MAX = ESP-NOW
  unsigned long hit_time_ms;   ///< millis() 時間戳
};

struct GameRecord {
  unsigned long start_time_ms;           ///< beep 後計時起點
  unsigned long stop_time_ms;            ///< 最後一槍或 forceStop 時間
  uint8_t       hit_count;               ///< 本局有效命中數
  HitRecord     hits[Limits::HIT_MAX];
  AppMode       mode;                    ///< 本局使用的模式
};

// --- Preset（槍型 / 靈敏度預設組）---
struct Preset {
  char      name[Limits::NAME_LEN];  ///< e.g. "Air Pistol", "Rifle"
  HitSource hitSource;
  int16_t   micThresh;               ///< 該槍型對應的 RMS 門檻
  uint32_t  cooldownMs;              ///< 冷卻時間（ms）
};

// --- Drill 定義（訓練課題）---
struct DrillDef {
  char        name[Limits::NAME_LEN]; ///< e.g. "6 shots / 3.5s"
  uint8_t     shotCount;              ///< 目標發數
  uint32_t    parTimeMs;              ///< 目標完成時間（ms），0 = 不限
  uint8_t     passPct;                ///< 及格分段佔比（0–100）
  bool        randomDelay;
  DrillLevel  level;
};

// --- Drill 執行結果 ---
struct DrillResult {
  DrillDef    def;                        ///< 執行時的課題快照
  GameRecord  record;                     ///< 本次執行原始紀錄
  uint8_t     passCount;                  ///< 達標（≤ par split）發數
  float       score;                      ///< 0.0–100.0 百分比
  bool        passed;                     ///< passCount/shotCount >= passPct
};

// --- RTOS IPC 訊息 ---
struct AudioCmd {
  enum class Op : uint8_t { PLAY, STOP } op;
  char path[24];
};

struct AudioDone {
  bool ok;
};

/** 命中事件（MicTask / ESPNow → GameFSM） */
struct __attribute__((packed)) HitMsg {
  uint8_t       station_id;   ///< 0 = MIC
  unsigned long hit_time_ms;  ///< millis()
};

/** SSE 推播訊息（GameFSM → NetworkTask） */
struct __attribute__((packed)) SseMsg {
  uint32_t      hitIdx;     ///< 本局第幾槍（1-based，環形模式可超過 HIT_MAX）
  uint8_t       stationId;
  unsigned long elapsed;    ///< 從 start 到本槍（ms）
  unsigned long split;      ///< 從上一槍到本槍（ms）
};

/** Web 指令訊息（WebServer → GameFSM） */
struct UiCmdMsg {
  WebCmd   cmd;
  uint32_t beatMs = 0;  ///< DRY_SET_BEAT 用的 payload
  uint32_t parMs  = 0;  ///< SET_PAR 用的 payload
};


// ============================================================
// [C15] 安全輔助巨集
//       用於邊界檢查、斷言，不依賴 C++ 例外
// ============================================================

/// 靜態斷言：編譯期確認結構大小符合預期（避免 padding 問題）
#define TT2_STATIC_ASSERT(cond, msg)  static_assert((cond), msg)

/// 執行期斷言：條件不成立時印出位置並重啟
/// 正式版可改為記錄 NVS 錯誤碼後重啟
#define TT2_ASSERT(cond)                          \
  do {                                            \
    if (!(cond)) {                                \
      Serial.printf("[ASSERT] %s:%d  %s\n",       \
                    __FILE__, __LINE__, #cond);   \
      ESP.restart();                              \
    }                                             \
  } while(0)

/// 邊界守衛：若 idx 超出 [0, limit) 則執行 action
#define TT2_BOUNDS(idx, limit, action)            \
  do {                                            \
    if ((idx) >= (limit)) {                       \
      Serial.printf("[BOUNDS] idx=%d limit=%d @%s:%d\n", \
                    (int)(idx), (int)(limit),     \
                    __FILE__, __LINE__);           \
      action;                                     \
    }                                             \
  } while(0)

/// 空指標守衛：若 ptr == nullptr 則執行 action
#define TT2_NOT_NULL(ptr, action)                 \
  do {                                            \
    if ((ptr) == nullptr) {                       \
      Serial.printf("[NULL] %s @%s:%d\n",         \
                    #ptr, __FILE__, __LINE__);    \
      action;                                     \
    }                                             \
  } while(0)

/// Queue 安全傳送（帶 timeout，失敗時僅記錄 warning，不 crash）
#define TT2_QUEUE_SEND(q, item, timeout_ms)       \
  do {                                            \
    if (xQueueSend((q), (item),                   \
        pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {  \
      Serial.printf("[Q] Send timeout @%s:%d\n", \
                    __FILE__, __LINE__);          \
    }                                             \
  } while(0)


// ============================================================
// [C16] 編譯期結構大小驗證（提前暴露 alignment 問題）
// 所有涉及 uint8_t + unsigned long 的結構均已加 __attribute__((packed))
// ESP32: unsigned long = 4 bytes, uint8_t = 1 byte
// ============================================================
TT2_STATIC_ASSERT(sizeof(HitRecord)  == 5,  "HitRecord size mismatch");   // 1+4
TT2_STATIC_ASSERT(sizeof(HitMsg)     == 5,  "HitMsg size mismatch");       // 1+4
TT2_STATIC_ASSERT(sizeof(SseMsg)     == 13, "SseMsg size mismatch");       // 4+1+4+4 (hitIdx=uint32_t)
TT2_STATIC_ASSERT(sizeof(AudioCmd)   <= 28, "AudioCmd too large for queue");
TT2_STATIC_ASSERT(Limits::HIT_MAX   <= 50,  "HIT_MAX exceeds safe range");
TT2_STATIC_ASSERT(Limits::PRESET_MAX <= 16, "PRESET_MAX exceeds NVS budget");
