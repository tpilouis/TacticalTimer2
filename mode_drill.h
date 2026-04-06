/**
 * @file    mode_drill.h
 * @brief   TACTICAL TIMER 2 — Drills 模式宣告
 *
 * 功能：
 *  - 設定射擊課題（發數 / Par Time / 及格門檻 / 隨機延遲）
 *  - 執行課題並即時評分（每槍判定 Pass/Fail）
 *  - 計算總分並顯示結果（通過率 / 平均分段）
 *  - 課題結果存入 SD session
 *
 * 畫面流程：SETUP → RUNNING → RESULT
 *
 * 按鍵對應（SETUP 畫面）：
 *  BtnA — 調小發數 / 返回 Home
 *  BtnB — 進入課題設定（觸控調整）
 *  BtnC — 開始課題
 *
 * 按鍵對應（RUNNING 畫面）：
 *  BtnA — 強制停止
 *  BtnC — 重新開始（同一課題）
 *
 * @version 2.0.0
 */

#pragma once

#include "game_fsm.h"


class ModeDrill : public ModeBase {
public:
  void    onEnter() override;
  void    update()  override;
  void    onExit()  override;
  AppMode getMode() const override { return AppMode::DRILL; }
  void    onButton(uint8_t btn) override;
  void    onTouch(int16_t x, int16_t y) override;

  /// 供 WebServer 讀取目前課題設定（nullptr = 不在 DRILL 模式）
  static const DrillDef* getDef();

  /// Web 按 Start：重新從 AppSettings 載入設定後啟動（無論目前 phase）
  static void webStart();

  /// Web 更新設定後同步到 _def（若目前在 DRILL mode）
  static void updateDef(uint8_t shots, uint32_t parMs, uint8_t passPct);

private:
  enum class DrillPhase : uint8_t { SETUP, RUNNING, RESULT, REVIEW };
  DrillPhase _phase = DrillPhase::SETUP;

  DrillDef    _def;
  DrillResult _result;
  uint8_t     _passCount = 0;

  void drawSetupScreen();
  void drawResultScreen();
  void drawReviewScreen();
  void startRun();
  void saveDef();   ///< 儲存 _def 到 NVS 並推播 SSE

  static void onHit(uint32_t hitIdx, uint8_t stationId,
                    unsigned long elapsed, unsigned long split);
  static void onReadyDone();
  static void onBeepDone();
  static void onClearDone();
  static void onDelayExpired(GameState pending);

  bool judgeShot(uint32_t hitIdx, unsigned long split) const;
};
