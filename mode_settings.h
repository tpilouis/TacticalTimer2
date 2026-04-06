/**
 * @file    mode_settings.h
 * @brief   TACTICAL TIMER 2 — Settings 模式宣告
 *
 * 功能：
 *  - Preset 切換（5 組，左右滑動選取）
 *  - Hit Source 切換（MIC / ESP-NOW / BOTH）
 *  - MIC Threshold 調整（+/- 200 步進）
 *  - Random Delay ON/OFF
 *  - SAVE & EXIT 儲存並返回 HOME
 *
 * @version 2.0.0
 */

#pragma once

#include "game_fsm.h"


class ModeSettings : public ModeBase {
public:
  void    onEnter() override;
  void    update()  override;
  void    onExit()  override;
  AppMode getMode() const override { return AppMode::SETTINGS; }
  void    onButton(uint8_t btn) override;
  void    onTouch(int16_t x, int16_t y) override;

private:
  // 暫存編輯中的設定值（CANCEL 時可丟棄）
  AppSettings _edit;

  void redraw();           // 重繪整個設定畫面
  void saveAndExit();      // 儲存並返回 HOME
};
