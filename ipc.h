/**
 * @file    ipc.h
 * @brief   TACTICAL TIMER 2 — RTOS IPC 原語宣告
 *
 * 職責（僅此模組）：
 *  1. 宣告所有跨 Task 通訊所需的 Queue、Mutex、EventGroup 句柄
 *  2. 提供統一的建立（createAll）與刪除（deleteAll）介面
 *  3. 提供麥克風 I2S 時分複用的協調函式（Audio ↔ Mic 握手）
 *  4. 提供 Web → GameFSM 的指令路由函式
 *
 * IPC 拓撲（→ = 寫入方向）：
 *
 *  audioQueue     : UITask → AudioTask    PLAY/STOP 指令
 *  audioDoneQueue : AudioTask → UITask    播放完成通知
 *  hitQueue       : MicTask/ESPNow → GameFSM   命中事件
 *  sseQueue       : GameFSM → NetworkTask  Web SSE 推播資料
 *  uiCmdQueue     : WebServer → GameFSM    遠端控制指令
 *  stateMutex     : 所有 Task              保護 GameState / counters
 *  micCtrlEvent   : AudioTask ↔ MicTask    I2S 時分複用握手
 *
 * 安全原則：
 *  - 所有句柄在 createAll() 前為 nullptr；caller 負責先呼叫 createAll()
 *  - Queue 傳送統一使用帶 timeout 的 xQueueSend，失敗只警告不 crash
 *  - Mutex 使用 xSemaphoreTake/Give 配對，持有時間盡量短
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"


namespace IPC {

  // ──────────────────────────────────────────────────────────
  // [I1] Queue 句柄（extern 宣告，實例在 ipc.cpp）
  // ──────────────────────────────────────────────────────────

  /// UITask → AudioTask：PLAY / STOP 指令
  extern QueueHandle_t audioQueue;

  /// AudioTask → UITask/GameFSM：播放完成通知
  extern QueueHandle_t audioDoneQueue;

  /// MicTask / ESPNow → GameFSM：命中事件
  extern QueueHandle_t hitQueue;

  /// GameFSM → NetworkTask：Web SSE 推播資料
  extern QueueHandle_t sseQueue;

  /// WebServer → GameFSM：遠端控制指令（TT2 新增）
  extern QueueHandle_t uiCmdQueue;


  // ──────────────────────────────────────────────────────────
  // [I2] Mutex 句柄
  // ──────────────────────────────────────────────────────────

  /**
   * 保護以下共享狀態：
   *  - GameState（gameState）
   *  - 命中計數器（hitIdx, gameIdx）
   *  - GameRecord 寫入
   *
   * 使用規則：
   *  - 持有時間 < 1ms（僅讀寫值，不做 I/O）
   *  - 不得在持有 mutex 時呼叫任何 blocking 函式
   */
  extern SemaphoreHandle_t stateMutex;


  // ──────────────────────────────────────────────────────────
  // [I3] EventGroup 句柄
  // ──────────────────────────────────────────────────────────

  /**
   * micCtrlEvent：Audio ↔ Mic I2S 時分複用握手
   *
   * BIT_0  MIC_PAUSE_BIT     : AudioTask → MicTask「請暫停 PDM，釋放 I2S」
   * BIT_1  MIC_SUSPENDED_BIT : MicTask → AudioTask「已暫停，I2S 已釋放」
   *
   * 流程：
   *  Audio 播放前：setMicPauseBit() → waitMicSuspended()
   *  Audio 播放後：releaseMicPause()（清除兩個 bit）
   *  Mic  每幀前：if (isMicPauseRequested()) → confirmMicSuspended() → 等待清除
   */
  extern EventGroupHandle_t micCtrlEvent;

  constexpr EventBits_t MIC_PAUSE_BIT     = (EventBits_t)(1 << 0);
  constexpr EventBits_t MIC_SUSPENDED_BIT = (EventBits_t)(1 << 1);


  // ──────────────────────────────────────────────────────────
  // [I4] 生命週期
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 建立所有 IPC 原語（在 setup() 最前面呼叫）
   *
   * 任何一個建立失敗都會呼叫 Panic::halt()，系統重啟。
   * 成功後所有句柄保證非 nullptr。
   */
  void createAll();

  /**
   * @brief 釋放所有 IPC 原語（正常不會用到，供測試 / 重置用）
   */
  void deleteAll();


  // ──────────────────────────────────────────────────────────
  // [I5] Mic ↔ Audio 協調函式（AudioTask 呼叫）
  // ──────────────────────────────────────────────────────────

  /**
   * @brief AudioTask：要求 MicTask 暫停並等待確認
   *
   * 設置 MIC_PAUSE_BIT，然後等待 MIC_SUSPENDED_BIT 被設置。
   * 最大等待 200ms；超時仍繼續（Mic 可能不在 GOING 中）。
   */
  void requestMicPause();

  /**
   * @brief AudioTask：通知 MicTask 可以恢復
   *
   * 清除 MIC_PAUSE_BIT 和 MIC_SUSPENDED_BIT。
   * MicTask 偵測到清除後自行重新安裝 PDM driver。
   */
  void releaseMicPause();


  // ──────────────────────────────────────────────────────────
  // [I6] Mic ↔ Audio 協調函式（MicTask 呼叫）
  // ──────────────────────────────────────────────────────────

  /**
   * @brief MicTask：查詢是否收到暫停請求
   * @return true = AudioTask 要求暫停（應立即停止採樣並釋放 I2S）
   */
  bool isMicPauseRequested();

  /**
   * @brief MicTask：通知 AudioTask「I2S 已釋放，可以開始播放」
   *
   * 設置 MIC_SUSPENDED_BIT。
   */
  void confirmMicSuspended();

  /**
   * @brief MicTask：等待暫停請求被清除（即 AudioTask 播放完畢）
   *
   * 阻塞直到 MIC_PAUSE_BIT 被清除，或超時 5000ms。
   * @return true = 正常恢復，false = 超時
   */
  bool waitMicResume(uint32_t timeoutMs = 5000);


  // ──────────────────────────────────────────────────────────
  // [I7] 安全 Queue 操作 wrapper
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 安全傳送到 audioQueue（帶 timeout）
   * @param cmd  AudioCmd 結構
   * @return true = 成功
   */
  bool sendAudioCmd(const AudioCmd& cmd, uint32_t timeoutMs = 10);

  /**
   * @brief 安全傳送到 hitQueue（帶 timeout，來自 ISR / Task 皆可）
   * @param msg  HitMsg 結構
   * @return true = 成功
   */
  bool sendHit(const HitMsg& msg, uint32_t timeoutMs = 5);

  /**
   * @brief 安全傳送到 sseQueue（帶 timeout）
   * @param msg  SseMsg 結構
   * @return true = 成功
   */
  bool sendSse(const SseMsg& msg, uint32_t timeoutMs = 5);

  /**
   * @brief 安全傳送到 uiCmdQueue（帶 timeout，WebServer callback 呼叫）
   * @param cmd  UiCmdMsg 結構
   * @return true = 成功
   */
  bool sendUiCmd(const UiCmdMsg& cmd, uint32_t timeoutMs = 10);

  /**
   * @brief 嘗試從 audioDoneQueue 取出完成通知（非阻塞）
   * @param done  輸出參數
   * @return true = 有資料
   */
  bool receiveAudioDone(AudioDone& done);

  /**
   * @brief 嘗試從 hitQueue 取出命中事件（非阻塞）
   * @param msg  輸出參數
   * @return true = 有資料
   */
  bool receiveHit(HitMsg& msg);

  /**
   * @brief 嘗試從 uiCmdQueue 取出 Web 指令（非阻塞）
   * @param cmd  輸出參數
   * @return true = 有資料
   */
  bool receiveUiCmd(UiCmdMsg& cmd);

  /**
   * @brief 嘗試從 sseQueue 取出 SSE 訊息（非阻塞）
   * @param msg  輸出參數
   * @return true = 有資料
   */
  bool receiveSse(SseMsg& msg);

  // ──────────────────────────────────────────────────────────
  // [I8] Mutex 輔助：RAII 包裝
  // ──────────────────────────────────────────────────────────

  /**
   * @brief RAII Mutex Guard
   *
   * 用法：
   *   {
   *     IPC::MutexGuard guard(IPC::stateMutex);
   *     if (!guard.locked()) return;   // 取得 mutex 失敗（超時）
   *     // ... 安全存取共享資料 ...
   *   }  // 離開 scope 自動 release
   */
  class MutexGuard {
  public:
    explicit MutexGuard(SemaphoreHandle_t mtx, uint32_t timeoutMs = portMAX_DELAY)
      : _mtx(mtx)
      , _locked(xSemaphoreTake(mtx, pdMS_TO_TICKS(timeoutMs)) == pdTRUE)
    {}

    ~MutexGuard() {
      if (_locked && _mtx) {
        xSemaphoreGive(_mtx);
      }
    }

    /// @return true = mutex 已取得
    bool locked() const { return _locked; }

    // 禁止複製
    MutexGuard(const MutexGuard&)            = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

  private:
    SemaphoreHandle_t _mtx;
    bool              _locked;
  };

} // namespace IPC
