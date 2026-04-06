/**
 * @file    mode_free.h
 * @brief   TACTICAL TIMER 2 — Free Shooting 模式宣告
 *
 * 功能：
 *  - 標準射擊計時（AREYOUREADY → 隨機延遲 → BEEP → GOING）
 *  - Par Time 支援（GOING 中第二聲 beep 提醒）
 *  - 命中即時顯示（分段時間、累計時間）
 *  - 結束後更新最佳紀錄並儲存 session
 *
 * 按鍵對應：
 *  BtnA (STOP)    — forceStop()（GOING 中）/ 返回 Home（STOP 後）
 *  BtnB (SETTING) — 快速設定 Par Time（觸控滑桿）
 *  BtnC (START)   — startNew()（IDLE / STOP 狀態）
 *
 * @version 2.0.0
 */

#pragma once

#include "game_fsm.h"


class ModeFree : public ModeBase {
public:
  void    onEnter() override;
  void    update()  override;
  void    onExit()  override;
  AppMode getMode() const override { return AppMode::FREE; }
  void    onButton(uint8_t btn) override;
  void    onTouch(int16_t x, int16_t y) override;

  /**
   * @brief Web 設定 Par Time 後同步到 Core2 螢幕（在 loop Task 執行）
   *
   * 由 GameFSM::processWebCmd 在 loop Task 中呼叫，執行緒安全。
   * 若目前在 PAR SET 畫面則立即重繪；否則只更新暫存值。
   *
   * @param ms  新的 Par Time（ms）
   */
  static void applySetPar(uint32_t ms);

private:
  // ── 靜態 callback（傳入 TimerCore，不能捕獲 this）──────────
  static void onHit(uint32_t hitIdx, uint8_t stationId,
                    unsigned long elapsed, unsigned long split);
  static void onReadyDone();
  static void onBeepDone();
  static void onClearDone();
  static void onDelayExpired(GameState pending);
  static void onParExpired();

  // ── 局部狀態 ────────────────────────────────────────────────
  bool     _resultShown = false;  ///< STOP 後是否已顯示結果畫面
  bool     _inParSet    = false;  ///< 正在 Par Time 設定畫面
  uint32_t _parSetMs    = 0;      ///< 設定畫面中暫存的 par time（ms）

  // Par Set 畫面觸控處理（內部用）
  void handleParSetTouch(int16_t x, int16_t y);
  void enterParSet();
  void redrawShotPage();   ///< 只繪製當前頁 shot rows（供 onClearDone 用）
  void redrawFullPage();   ///< 完整重繪（header + rows + stats + btn bar）
  void exitParSet(bool save);
};
