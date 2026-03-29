/**
 * @file    timer_core.h
 * @brief   TACTICAL TIMER 2 — 計時核心服務宣告
 *
 * 職責：
 *  - 管理射擊計時的共享狀態（GameState、GameRecord、計數器）
 *  - 提供 thread-safe 狀態讀寫（封裝 IPC::stateMutex）
 *  - 計算 split / elapsed / par time 判定
 *  - 維護本次開機的最佳紀錄（bestTotalSec / bestAvgSec）
 *  - 隨機延遲 / Par Time 計時管理
 *
 * 不包含：Display、Audio、Web 推播（由上層 FSM 透過 callback 協調）
 *
 * 設計模式：Repository（GameRecord CRUD） +
 *           Callback Hook（讓 GameFSM 注入事件處理）
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include "ipc.h"


namespace TimerCore {

  // ──────────────────────────────────────────────────────────
  // [T2] 生命週期
  // ──────────────────────────────────────────────────────────

  /** @brief 初始化計時核心（setup() 最前面呼叫，IPC::createAll() 之後） */
  void init();


  // ──────────────────────────────────────────────────────────
  // [T3] 遊戲流程控制（由 GameFSM 呼叫）
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 開始新局
   *
   * 分配新的 GameRecord slot，重置計數器，設定狀態 AREYOUREADY。
   * 若 gameIdx 達到 GAMES_MAX，環型覆蓋最舊紀錄。
   * @return 分配到的 gameIdx（1-based）
   */
  uint8_t startNew();

  /**
   * @brief 記錄計時起點（beep 播完後、切換 GOING 前呼叫）
   *
   * 設定 GameRecord.start_time_ms = millis()，
   * 計算 goingOpenAt = millis() + guardMs。
   * @param guardMs  保護窗口（ms），預設 Timing::GOING_GUARD_MS
   */
  void markStart(uint32_t guardMs = Timing::GOING_GUARD_MS);

  /**
   * @brief 強制停止目前局（STOP 按鍵或 Web STOP 指令）
   *
   * 停止 par time 計時，設定狀態 SHOWCLEAR，啟動 AFTER_CLEAR_DELAY。
   */
  void forceStop();

  /**
   * @brief 處理命中事件佇列（在 loop() 每幀呼叫）
   *
   * 守衛：gameState==GOING、goingOpenAt、gameIdx>0、hitIdx<HIT_MAX、雙重確認。
   *
   * @param onHitCb  命中成功回呼 (hitIdx_1based, stationId, elapsed_ms, split_ms)
   *                 nullptr = 略過回呼
   */
  void processHits(void (*onHitCb)(uint8_t, uint8_t,
                                   unsigned long, unsigned long) = nullptr);

  /**
   * @brief 處理音訊完成通知（audioDoneQueue，在 loop() 每幀呼叫）
   *
   * 依目前 GameState 決定下一步：
   *   AREYOUREADY → 啟動隨機/固定延遲（onReadyDoneCb）
   *   BEEPING     → markStart()，切換 GOING（onBeepDoneCb）
   *   SHOWCLEAR   → 更新最佳，切換 STOP（onClearDoneCb）
   */
  void processAudioDone(void (*onReadyDoneCb)() = nullptr,
                        void (*onBeepDoneCb)()  = nullptr,
                        void (*onClearDoneCb)() = nullptr);

  /**
   * @brief 處理延遲計時（在 loop() 每幀呼叫）
   *
   * 延遲結束後呼叫 onExpiredCb(pendingState)，由 FSM 播放對應音效。
   */
  void processDelay(void (*onExpiredCb)(GameState) = nullptr);

  /**
   * @brief 處理 par time 計時（在 loop() GOING 期間呼叫）
   *
   * Par time 到期後呼叫 onParCb()，由 FSM 播放第二聲 beep。
   */
  void processParTime(void (*onParCb)() = nullptr);

  /**
   * @brief 處理 Web 遠端指令（uiCmdQueue，在 loop() 每幀呼叫）
   */
  void processWebCmd(void (*onStartCb)()           = nullptr,
                     void (*onStopCb)()            = nullptr,
                     void (*onSetBeatCb)(uint32_t) = nullptr,
                     void (*onSpyClearCb)()        = nullptr,
                     void (*onSetParCb)(uint32_t)  = nullptr);


  // ──────────────────────────────────────────────────────────
  // [T4] 狀態查詢（thread-safe）
  // ──────────────────────────────────────────────────────────

  GameState     getState();
  void          setState(GameState s);
  uint8_t       getGameIdx();
  uint8_t       getHitIdx();
  unsigned long getGoingOpenAt();

  /**
   * @brief 取得 GameRecord（只讀）
   * @param reverseIdx  0 = 最近一局，1 = 上一局，依此類推
   * @return const 指標；nullptr = 無效（尚無紀錄）
   */
  const GameRecord* getRecord(uint8_t reverseIdx = 0);

  float getBestTotal();
  float getBestAvg();

  /**
   * @brief 嘗試更新最佳紀錄
   * @return true = 至少一項創新高
   */
  bool updateBest(float totalSec, float avgSec);


  // ──────────────────────────────────────────────────────────
  // [T5] 延遲 / Par Time 設定
  // ──────────────────────────────────────────────────────────

  void     setRandomDelay(bool enabled);
  void     setParTime(uint32_t ms);
  uint32_t getParTime();
  bool     isRandomDelay();


  // ──────────────────────────────────────────────────────────
  // [T6] 計算輔助（純函式，可在任意 Task 安全呼叫）
  // ──────────────────────────────────────────────────────────

  unsigned long calcElapsed(const GameRecord& rec, uint8_t hitIdx1based);
  unsigned long calcSplit  (const GameRecord& rec, uint8_t hitIdx1based);
  unsigned long calcTotalMs(const GameRecord& rec);

} // namespace TimerCore
