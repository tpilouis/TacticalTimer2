/**
 * @file    safety.cpp
 * @brief   TACTICAL TIMER 2 — 安全防護層實作
 *
 * 實作 safety.h 宣告的所有安全輔助函式，包含：
 *  - 錯誤碼名稱對照表
 *  - ESP32 Task Watchdog 管理
 *  - 恐慌處理（錯誤記錄 + 安全重啟）
 *  - 記憶體健康監控
 *
 * @version 2.0.0
 */

#include "safety.h"
#include <Preferences.h>


// ============================================================
// [S1-impl] 錯誤碼名稱對照
// ============================================================

/**
 * @brief 將 TT2Error 枚舉值轉為可讀字串
 *
 * 用於 Serial 輸出診斷資訊，回傳靜態字串常數，不做動態分配。
 *
 * @param e  TT2Error 錯誤碼
 * @return   錯誤名稱字串（永不為 nullptr）
 */
const char* tt2ErrorName(TT2Error e) {
  switch (e) {
    case TT2Error::OK:               return "OK";
    case TT2Error::IPC_QUEUE_FULL:   return "IPC_QUEUE_FULL";
    case TT2Error::IPC_QUEUE_NULL:   return "IPC_QUEUE_NULL";
    case TT2Error::IPC_MUTEX_NULL:   return "IPC_MUTEX_NULL";
    case TT2Error::IPC_CREATE_FAIL:  return "IPC_CREATE_FAIL";
    case TT2Error::AUDIO_I2S_FAIL:   return "AUDIO_I2S_FAIL";
    case TT2Error::AUDIO_FILE_404:   return "AUDIO_FILE_404";
    case TT2Error::AUDIO_PLAY_FAIL:  return "AUDIO_PLAY_FAIL";
    case TT2Error::MIC_INSTALL_FAIL: return "MIC_INSTALL_FAIL";
    case TT2Error::MIC_READ_FAIL:    return "MIC_READ_FAIL";
    case TT2Error::MIC_TIMEOUT:      return "MIC_TIMEOUT";
    case TT2Error::SD_MOUNT_FAIL:    return "SD_MOUNT_FAIL";
    case TT2Error::SD_WRITE_FAIL:    return "SD_WRITE_FAIL";
    case TT2Error::SD_READ_FAIL:     return "SD_READ_FAIL";
    case TT2Error::NVS_OPEN_FAIL:    return "NVS_OPEN_FAIL";
    case TT2Error::NVS_WRITE_FAIL:   return "NVS_WRITE_FAIL";
    case TT2Error::WIFI_TIMEOUT:     return "WIFI_TIMEOUT";
    case TT2Error::ESPNOW_INIT_FAIL: return "ESPNOW_INIT_FAIL";
    case TT2Error::NTP_SYNC_FAIL:    return "NTP_SYNC_FAIL";
    case TT2Error::FSM_BAD_STATE:    return "FSM_BAD_STATE";
    case TT2Error::FSM_BAD_MODE:     return "FSM_BAD_MODE";
    case TT2Error::BOUNDS_HIT:       return "BOUNDS_HIT";
    case TT2Error::BOUNDS_GAME:      return "BOUNDS_GAME";
    case TT2Error::BOUNDS_PRESET:    return "BOUNDS_PRESET";
    case TT2Error::MEM_LOW:          return "MEM_LOW";
    case TT2Error::STACK_OVERFLOW:   return "STACK_OVERFLOW";
    default:                         return "UNKNOWN";
  }
}


// ============================================================
// [S2-impl] 看門狗
// ============================================================

/**
 * @brief 初始化 ESP32 Task Watchdog
 *
 * panic=true：WDT 超時直接觸發 ESP-IDF 恐慌重啟，
 * 可在 Serial 看到 backtrace，方便除錯。
 *
 * @note 必須在 setup() 最前面、所有 Task 建立之前呼叫
 */
void WDT::init() {
  esp_task_wdt_init(WDT::TIMEOUT_S, /*panic=*/true);
  Serial.printf("[WDT] Initialized, timeout=%ds\n", WDT::TIMEOUT_S);
}

/**
 * @brief 訂閱目前 Task 到看門狗
 *
 * 訂閱後必須定期呼叫 WDT::feed()，否則超時重啟。
 * 在每個 RTOS Task 進入主迴圈前呼叫。
 */
void WDT::subscribe() {
  esp_err_t r = esp_task_wdt_add(nullptr);
  if (r != ESP_OK) {
    Serial.printf("[WDT] Subscribe failed: 0x%x\n", r);
  }
}

/**
 * @brief 餵狗（重置 WDT 計時器）
 *
 * 在 Task 主迴圈定期呼叫，建議每 1–2 秒一次。
 */
void WDT::feed() {
  esp_task_wdt_reset();
}

/**
 * @brief 取消目前 Task 的看門狗訂閱
 *
 * Task 正常結束前呼叫，避免 WDT 誤判死亡。
 */
void WDT::unsubscribe() {
  esp_task_wdt_delete(nullptr);
}


// ============================================================
// [S3-impl] 恐慌處理
// ============================================================

/// @cond INTERNAL
static constexpr char PANIC_NVS_NS[]  = "tt2_panic";
static constexpr char PANIC_NVS_ERR[] = "last_err";
static constexpr char PANIC_NVS_MSG[] = "last_msg";
/// @endcond

/**
 * @brief 記錄錯誤原因並執行安全重啟（不返回）
 *
 * 執行流程：
 *  1. 停止看門狗，避免 WDT 干擾重啟流程
 *  2. Serial 輸出完整錯誤資訊
 *  3. 嘗試將錯誤碼與訊息寫入 NVS（重啟後可讀取診斷）
 *  4. delay(1000) 確保 Serial flush
 *  5. ESP.restart()
 *
 * @param err   TT2Error 錯誤碼
 * @param msg   附加說明字串（可為 nullptr）
 */
[[noreturn]] void Panic::halt(TT2Error err, const char* msg) {
  esp_task_wdt_delete(nullptr);

  Serial.printf("\n[PANIC] Error=0x%04X (%s)\n",
                static_cast<uint16_t>(err), tt2ErrorName(err));
  if (msg) {
    Serial.printf("[PANIC] %s\n", msg);
  }
  Serial.println("[PANIC] System will restart in 1s...");

  Preferences prefs;
  if (prefs.begin(PANIC_NVS_NS, false)) {
    prefs.putUShort(PANIC_NVS_ERR, static_cast<uint16_t>(err));
    if (msg) {
      prefs.putString(PANIC_NVS_MSG, msg);
    }
    prefs.end();
  }

  delay(1000);
  ESP.restart();
  while (true) { vTaskDelay(1); }
}

/**
 * @brief 非致命錯誤記錄（僅 Serial 輸出，不重啟）
 *
 * 適用於可容忍的錯誤，如 SD 寫入失敗、Queue 傳送逾時等。
 *
 * @param err   TT2Error 錯誤碼
 * @param msg   附加說明字串（可為 nullptr）
 */
void Panic::warn(TT2Error err, const char* msg) {
  Serial.printf("[WARN] Error=0x%04X (%s)",
                static_cast<uint16_t>(err), tt2ErrorName(err));
  if (msg) {
    Serial.printf(" : %s", msg);
  }
  Serial.println();
}


// ============================================================
// [S4-impl] 記憶體健康監控
// ============================================================

/**
 * @brief 檢查 heap 是否健康
 *
 * 若目前可用 heap 低於 FREE_HEAP_MIN（20KB），輸出警告。
 *
 * @return true  heap 正常
 * @return false heap 不足
 */
bool MemCheck::heapOk() {
  uint32_t free = ESP.getFreeHeap();
  if (free < FREE_HEAP_MIN) {
    Serial.printf("[MEM] Low heap: %u bytes (min=%u)\n",
                  free, FREE_HEAP_MIN);
    return false;
  }
  return true;
}

/**
 * @brief 印出目前 heap / PSRAM 狀態
 *
 * 輸出格式：`[MEM][tag] heap=xxx minHeap=xxx psram=xxx`
 *
 * @param tag  呼叫者標識字串，用於區分輸出來源（可為 nullptr）
 */
void MemCheck::report(const char* tag) {
  Serial.printf("[MEM][%s] heap=%u  minHeap=%u  psram=%u\n",
                tag ? tag : "?",
                ESP.getFreeHeap(),
                ESP.getMinFreeHeap(),
                ESP.getFreePsram());
}

/**
 * @brief 檢查指定 Task 的 stack 剩餘空間
 *
 * 透過 FreeRTOS uxTaskGetStackHighWaterMark() 取得 stack 最低水位，
 * 低於 minWords 時輸出警告。
 *
 * @param task      目標 TaskHandle_t，nullptr = 目前執行中的 Task
 * @param minWords  警告閾值（FreeRTOS word 單位，ESP32 = 4 bytes/word）
 */
void MemCheck::checkStack(TaskHandle_t task, uint32_t minWords) {
  TaskHandle_t t = task ? task : xTaskGetCurrentTaskHandle();
  UBaseType_t watermark = uxTaskGetStackHighWaterMark(t);
  if (watermark < minWords) {
    Serial.printf("[STACK] Low watermark: %u words (min=%u) task=%s\n",
                  (unsigned)watermark, (unsigned)minWords,
                  pcTaskGetTaskName(t));
  }
}
