/**
 * @file    ipc.cpp
 * @brief   TACTICAL TIMER 2 — RTOS IPC 原語實作
 *
 * 定義所有 Queue / Mutex / EventGroup 句柄的全域實例，
 * 並實作建立、刪除、協調與安全傳送等函式。
 *
 * @version 2.0.0
 */

#include "ipc.h"


// ============================================================
// [I1-impl] 句柄實例（全域，僅此檔案定義）
// ============================================================
namespace IPC {
  QueueHandle_t      audioQueue     = nullptr;  ///< UITask → AudioTask
  QueueHandle_t      audioDoneQueue = nullptr;  ///< AudioTask → UITask
  QueueHandle_t      hitQueue       = nullptr;  ///< MicTask/ESPNow → GameFSM
  QueueHandle_t      sseQueue       = nullptr;  ///< GameFSM → NetworkTask
  QueueHandle_t      uiCmdQueue     = nullptr;  ///< WebServer → GameFSM
  SemaphoreHandle_t  stateMutex     = nullptr;  ///< 保護 GameState / counters
  EventGroupHandle_t micCtrlEvent   = nullptr;  ///< Audio ↔ Mic I2S 握手
}


// ============================================================
// [I4-impl] 生命週期
// ============================================================

/**
 * @brief 建立所有 RTOS IPC 原語
 *
 * 依序建立 5 個 Queue、1 個 Mutex、1 個 EventGroup。
 * 任一建立失敗即呼叫 Panic::halt()，因為系統無法在缺少 IPC 的情況下安全運作。
 *
 * @note 必須在 setup() 最前面、所有 Task 建立之前呼叫
 */
void IPC::createAll() {
  audioQueue     = xQueueCreate(QDepth::Q_AUDIO,      sizeof(AudioCmd));
  audioDoneQueue = xQueueCreate(QDepth::Q_AUDIO_DONE, sizeof(AudioDone));
  hitQueue       = xQueueCreate(QDepth::Q_HIT,        sizeof(HitMsg));
  sseQueue       = xQueueCreate(QDepth::Q_SSE,        sizeof(SseMsg));
  uiCmdQueue     = xQueueCreate(QDepth::Q_UI_CMD,     sizeof(UiCmdMsg));
  stateMutex     = xSemaphoreCreateMutex();
  micCtrlEvent   = xEventGroupCreate();

  // 任一建立失敗 → 致命錯誤，系統無法安全運作
  if (!audioQueue     || !audioDoneQueue || !hitQueue   ||
      !sseQueue       || !uiCmdQueue     || !stateMutex ||
      !micCtrlEvent) {
    // 在 Serial 可用前先記錄，再呼叫 Panic
    Serial.println("[IPC] FATAL: createAll failed");
    Panic::halt(TT2Error::IPC_CREATE_FAIL, "IPC::createAll failed");
  }

  Serial.println("[IPC] All primitives created OK");
}

/**
 * @brief 釋放所有 RTOS IPC 原語
 *
 * 將所有句柄刪除並設為 nullptr，確保後續誤用能被 TT2_NOT_NULL 捕捉。
 * 正常執行不需呼叫，僅供測試或系統重置使用。
 */
void IPC::deleteAll() {
  if (audioQueue)     { vQueueDelete(audioQueue);     audioQueue     = nullptr; }
  if (audioDoneQueue) { vQueueDelete(audioDoneQueue); audioDoneQueue = nullptr; }
  if (hitQueue)       { vQueueDelete(hitQueue);       hitQueue       = nullptr; }
  if (sseQueue)       { vQueueDelete(sseQueue);       sseQueue       = nullptr; }
  if (uiCmdQueue)     { vQueueDelete(uiCmdQueue);     uiCmdQueue     = nullptr; }
  if (stateMutex)     { vSemaphoreDelete(stateMutex); stateMutex     = nullptr; }
  if (micCtrlEvent)   { vEventGroupDelete(micCtrlEvent); micCtrlEvent = nullptr; }
}


// ============================================================
// [I5-impl] Mic ↔ Audio 協調（AudioTask 呼叫）
// ============================================================

/**
 * @brief AudioTask：設置暫停旗標並等待 MicTask 確認
 *
 * 設置 MIC_PAUSE_BIT，然後等待 MIC_SUSPENDED_BIT 被 MicTask 設置。
 * 最大等待 200ms；超時仍繼續（MicTask 可能不在採樣中，屬正常情況）。
 */
void IPC::requestMicPause() {
  TT2_NOT_NULL(micCtrlEvent, return);

  // 發出暫停請求
  xEventGroupSetBits(micCtrlEvent, MIC_PAUSE_BIT);

  // 等待 MicTask 確認已暫停（BIT_1），最多 200ms
  // pdFALSE = 不清除 bit（讓 MicTask 自己管理）
  EventBits_t bits = xEventGroupWaitBits(
    micCtrlEvent,
    MIC_SUSPENDED_BIT,
    pdFALSE,   // 不自動清除
    pdTRUE,    // 等待 ALL bits（此處只有一個 bit）
    pdMS_TO_TICKS(200)
  );

  if (!(bits & MIC_SUSPENDED_BIT)) {
    // 超時：MicTask 可能不在 GOING 狀態（不是錯誤，只是警告）
    Panic::warn(TT2Error::MIC_TIMEOUT, "requestMicPause: MicTask did not confirm in 200ms");
  }
}

/**
 * @brief AudioTask：通知 MicTask 可以恢復採樣
 *
 * 清除 MIC_PAUSE_BIT 和 MIC_SUSPENDED_BIT。
 * MicTask 偵測到 PAUSE_BIT 消失後，自行重新安裝 PDM driver 並恢復採樣。
 */
void IPC::releaseMicPause() {
  TT2_NOT_NULL(micCtrlEvent, return);
  // 清除兩個 bit → MicTask 偵測到 PAUSE_BIT 消失後重新初始化 PDM
  xEventGroupClearBits(micCtrlEvent, MIC_PAUSE_BIT | MIC_SUSPENDED_BIT);
}


// ============================================================
// [I6-impl] Mic ↔ Audio 協調（MicTask 呼叫）
// ============================================================

/**
 * @brief MicTask：查詢是否收到 AudioTask 的暫停請求
 *
 * @return true  AudioTask 已設置 MIC_PAUSE_BIT，應立即停止採樣
 * @return false 無暫停請求，可正常採樣
 */
bool IPC::isMicPauseRequested() {
  if (!micCtrlEvent) return false;
  return (xEventGroupGetBits(micCtrlEvent) & MIC_PAUSE_BIT) != 0;
}

/**
 * @brief MicTask：通知 AudioTask「I2S 已釋放，可開始播放」
 *
 * 設置 MIC_SUSPENDED_BIT，AudioTask 在 requestMicPause() 中等待此 bit。
 */
void IPC::confirmMicSuspended() {
  TT2_NOT_NULL(micCtrlEvent, return);
  xEventGroupSetBits(micCtrlEvent, MIC_SUSPENDED_BIT);
}

/**
 * @brief MicTask：等待暫停請求被清除（即 AudioTask 播放完畢）
 *
 * 輪詢 MIC_PAUSE_BIT，每 10ms 檢查一次，直到 bit 被清除或超時。
 *
 * @param timeoutMs  最大等待時間（ms），預設 5000ms
 * @return true   MIC_PAUSE_BIT 已清除，可恢復採樣
 * @return false  等待超時（AudioTask 可能異常）
 */
bool IPC::waitMicResume(uint32_t timeoutMs) {
  TT2_NOT_NULL(micCtrlEvent, return false);

  // 等待 MIC_PAUSE_BIT 被清除（AudioTask 呼叫 releaseMicPause 後）
  // xEventGroupWaitBits 等待 bit SET，這裡我們需要等待 bit CLEAR
  // 做法：輪詢，每 10ms 檢查一次
  uint32_t deadline = (uint32_t)xTaskGetTickCount() + pdMS_TO_TICKS(timeoutMs);
  while ((xEventGroupGetBits(micCtrlEvent) & MIC_PAUSE_BIT) != 0) {
    if ((uint32_t)xTaskGetTickCount() >= deadline) {
      Panic::warn(TT2Error::MIC_TIMEOUT, "waitMicResume: timeout");
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}


// ============================================================
// [I7-impl] 安全 Queue 操作
// ============================================================

/**
 * @brief 安全傳送 AudioCmd 到 audioQueue
 * @param cmd        AudioCmd（PLAY / STOP + 檔案路徑）
 * @param timeoutMs  等待 Queue 空位的超時時間（ms）
 * @return true  傳送成功
 * @return false Queue 為 nullptr 或逾時
 */
bool IPC::sendAudioCmd(const AudioCmd& cmd, uint32_t timeoutMs) {
  TT2_NOT_NULL(audioQueue, return false);
  bool ok = (xQueueSend(audioQueue, &cmd, pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
  if (!ok) Panic::warn(TT2Error::IPC_QUEUE_FULL, "audioQueue full");
  return ok;
}

/**
 * @brief 安全傳送 HitMsg 到 hitQueue
 * @param msg        HitMsg（station_id + hit_time_ms）
 * @param timeoutMs  超時時間（ms）
 * @return true / false
 */
bool IPC::sendHit(const HitMsg& msg, uint32_t timeoutMs) {
  TT2_NOT_NULL(hitQueue, return false);
  bool ok = (xQueueSend(hitQueue, &msg, pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
  if (!ok) Panic::warn(TT2Error::IPC_QUEUE_FULL, "hitQueue full");
  return ok;
}

/**
 * @brief 安全傳送 SseMsg 到 sseQueue
 * @param msg        SseMsg（SSE 推播資料）
 * @param timeoutMs  超時時間（ms）
 * @return true / false
 */
bool IPC::sendSse(const SseMsg& msg, uint32_t timeoutMs) {
  TT2_NOT_NULL(sseQueue, return false);
  bool ok = (xQueueSend(sseQueue, &msg, pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
  if (!ok) Panic::warn(TT2Error::IPC_QUEUE_FULL, "sseQueue full");
  return ok;
}

/**
 * @brief 安全傳送 UiCmdMsg 到 uiCmdQueue
 * @param cmd        UiCmdMsg（WebCmd + 可選 payload）
 * @param timeoutMs  超時時間（ms）
 * @return true / false
 */
bool IPC::sendUiCmd(const UiCmdMsg& cmd, uint32_t timeoutMs) {
  TT2_NOT_NULL(uiCmdQueue, return false);
  bool ok = (xQueueSend(uiCmdQueue, &cmd, pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
  if (!ok) Panic::warn(TT2Error::IPC_QUEUE_FULL, "uiCmdQueue full");
  return ok;
}

/**
 * @brief 非阻塞取出 audioDoneQueue 的完成通知
 * @param[out] done  AudioDone 結構
 * @return true  有資料（播放完成）
 * @return false Queue 空
 */
bool IPC::receiveAudioDone(AudioDone& done) {
  TT2_NOT_NULL(audioDoneQueue, return false);
  return xQueueReceive(audioDoneQueue, &done, 0) == pdTRUE;
}

/**
 * @brief 非阻塞取出 hitQueue 的命中事件
 * @param[out] msg  HitMsg 結構
 * @return true  有資料
 * @return false Queue 空
 */
bool IPC::receiveHit(HitMsg& msg) {
  TT2_NOT_NULL(hitQueue, return false);
  return xQueueReceive(hitQueue, &msg, 0) == pdTRUE;
}

/**
 * @brief 非阻塞取出 uiCmdQueue 的 Web 指令
 * @param[out] cmd  UiCmdMsg 結構
 * @return true  有資料
 * @return false Queue 空
 */
bool IPC::receiveUiCmd(UiCmdMsg& cmd) {
  TT2_NOT_NULL(uiCmdQueue, return false);
  return xQueueReceive(uiCmdQueue, &cmd, 0) == pdTRUE;
}

/**
 * @brief 非阻塞取出 sseQueue 的 SSE 訊息
 * @param[out] msg  SseMsg 結構
 * @return true  有資料
 * @return false Queue 空
 */
bool IPC::receiveSse(SseMsg& msg) {
  TT2_NOT_NULL(sseQueue, return false);
  return xQueueReceive(sseQueue, &msg, 0) == pdTRUE;
}
