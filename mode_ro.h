/**
 * @file    mode_ro.h
 * @brief   TACTICAL TIMER 2 — RO（Range Officer）模式宣告
 *
 * 功能：
 *  - 發出指令音（含隨機延遲），記錄第一槍「拔槍時間（Draw Time）」
 *  - 後續槍視為 split，完整紀錄比對 par time
 *  - 適合競技射擊的現場裁判使用
 *
 * 顯示佈局：
 *  大字顯示 Draw Time（第一槍）
 *  下方依序列出 Split 1–9（後續槍）
 *
 * 按鍵：
 *  BtnA — 停止 / Home
 *  BtnB — 切換隨機/固定延遲
 *  BtnC — 開始（FIRE 指令）/ 重新開始
 *
 * @version 2.0.0
 */

#pragma once
#include "game_fsm.h"


class ModeRO : public ModeBase {
public:
  void    onEnter() override;
  void    update()  override;
  void    onExit()  override;
  AppMode getMode() const override { return AppMode::RO; }
  void    onButton(uint8_t btn) override;
  void    onTouch(int16_t x, int16_t y) override;

private:
  unsigned long _drawTimeMs  = 0;
  bool          _drawRecorded = false;

  // 內部輔助：只更新 header + Draw Time，不清 Shot Row
  static void roUpdateUI(const char* badge, uint16_t badgeColor,
                         const char* btnA, const char* btnB, const char* btnC);

  static void onHit(uint32_t hitIdx, uint8_t stationId,
                    unsigned long elapsed, unsigned long split);
  static void onReadyDone();
  static void onBeepDone();
  static void onClearDone();
  static void onDelayExpired(GameState pending);

  void drawScreen(const char* badge, uint16_t badgeColor);
  void redrawShotPage();  ///< 翻頁後重繪當前頁成績列
};
