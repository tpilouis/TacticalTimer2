/**
 * @file    mic_engine.h
 * @brief   TACTICAL TIMER 2 — 麥克風音爆偵測服務宣告
 *
 * 職責：
 *  - 管理 MicTask 的完整執行迴圈（RTOS Task 入口）
 *  - 協調 HalMic 的 driver 生命週期（暫停/恢復 I2S 時分複用）
 *  - 執行 RMS 計算與音爆偵測（threshold + cooldown）
 *  - 支援 Spy Mode（不限 GOING 狀態，持續偵測並記錄）
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include "ipc.h"
#include "hal_mic.h"
#include "timer_core.h"


namespace MicEngine {

  /**
   * @brief 更新偵測設定（可在 Task 執行中呼叫，atomic copy）
   * @param threshold  RMS 門檻
   * @param cooldownMs 冷卻時間（ms）
   * @param source     HitSource（決定是否啟動 Mic）
   */
  void setConfig(int16_t threshold, uint32_t cooldownMs, HitSource source);

  /**
   * @brief 啟用/停用 Spy Mode
   *
   * Spy Mode：不限 GOING 狀態，持續偵測記錄；命中送 hitQueue。
   */
  void setSpyMode(bool enabled);
  bool isSpyMode();

  /**
   * @brief MicTask 主函式（FreeRTOS task entry）
   *
   * 執行迴圈：
   *  [A] 模式守衛：source != MIC + !spyMode → 低頻休眠
   *  [B] 暫停握手：isMicPauseRequested → deinit → confirm → waitResume
   *  [C] Driver 確保安裝：!installed → init，失敗 retry 500ms
   *  [D] 採樣：HalMic::read()，失敗標記重新初始化
   *  [E] 二次暫停檢查（read 返回後）
   *  [F] RMS 計算
   *  [G] 狀態守衛：非 GOING 且非 SpyMode → continue
   *  [H] 保護窗口守衛（goingOpenAt，僅非 SpyMode）
   *  [I] 門檻觸發 + 冷卻 → sendHit
   */
  void micTaskFn(void* param);

} // namespace MicEngine
