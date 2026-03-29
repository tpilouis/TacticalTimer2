/**
 * @file    mode_dryfire.h
 * @brief   TACTICAL TIMER 2 — Dry Fire 節拍訓練模式宣告
 *
 * 功能：
 *  - 以固定或隨機間隔發出 beep，模擬射擊節奏
 *  - 不使用麥克風，純音效節拍
 *  - 可調整節拍間隔（BeatMs：500ms ~ 5000ms）
 *  - 顯示已完成節拍數和已用時間
 *
 * 按鍵：
 *  BtnA — 停止 / Home
 *  BtnB — 調慢（+200ms）
 *  BtnC — 開始 / 調快（-200ms）
 *
 * @version 2.0.0
 */

#pragma once
#include "game_fsm.h"


class ModeDryFire : public ModeBase {
public:
  void    onEnter() override;
  void    update()  override;
  void    onExit()  override;
  AppMode getMode() const override { return AppMode::DRY_FIRE; }
  void    onButton(uint8_t btn) override;
  void    onTouch(int16_t x, int16_t y) override;

  static uint32_t getBeatMs();
  static void     setBeatMs(uint32_t ms);
  static void     webStart();
  static void     webStop();
  static void     applySetBeat(uint32_t ms);
  static bool     isRunning();
  static void     doStart();
  static void     doStop();

private:
  bool          _running       = false;
  uint32_t      _beatMs        = Timing::DRY_BEAT_DEF_MS;
  unsigned long _lastBeatAt    = 0;
  uint32_t      _beatCount     = 0;
  unsigned long _sessionStart  = 0;

  void drawScreen();
  void startSession();
  void stopSession();
  void saveBeatMs();  ///< 儲存到 NVS 並推播 SSE
  void tick();
};
