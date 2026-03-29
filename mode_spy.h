/**
 * @file    mode_spy.h
 * @brief   TACTICAL TIMER 2 — Spy Mode（靜默旁觀）宣告
 *
 * 功能：
 *  - 不發出任何音效，靜默偵聽附近槍聲
 *  - 偵測到音爆時記錄時間戳與分段，顯示在螢幕上
 *  - 適合教練或 Range Officer 在旁監測射手成績
 *  - 螢幕亮度可調低（選擇性：Phase 5 UI 實作）
 *
 * 按鍵：
 *  BtnA — 停止並顯示統計 / 返回 Home
 *  BtnB — 清除紀錄（重新開始監聽）
 *  BtnC — 開始監聽 / 暫停
 *
 * @version 2.0.0
 */

#pragma once
#include "game_fsm.h"


class ModeSpy : public ModeBase {
public:
  void    onEnter() override;
  void    update()  override;
  void    onExit()  override;
  AppMode getMode() const override { return AppMode::SPY; }
  void    onButton(uint8_t btn) override;
  void    onTouch(int16_t x, int16_t y) override;
  bool    isListening() const { return _listening; }

private:
  bool          _listening     = false;
  uint8_t       _shotCount     = 0;
  unsigned long _listenStartAt = 0;  ///< 按下 LISTEN 的時間戳
  unsigned long _firstShotAt   = 0;
  unsigned long _lastShotAt    = 0;

  // Spy 模式的命中記錄（本地，不依賴 TimerCore）
  struct SpyHit {
    unsigned long timestamp;
    unsigned long split;
  };
  SpyHit _hits[Limits::HIT_MAX];

  void startListening();
  void stopListening();
  void drawScreen();

  static void onSpyHit(uint8_t hitIdx, uint8_t stationId,
                       unsigned long elapsed, unsigned long split);
};
