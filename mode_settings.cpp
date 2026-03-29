/**
 * @file    mode_settings.cpp
 * @brief   TACTICAL TIMER 2 — Settings 模式實作
 *
 * 提供 Core2 端的設定介面，包含：
 *  - Preset（槍型）切換
 *  - Detection Source（ESP-NOW / MIC / BOTH）
 *  - Mic Threshold 調整
 *  - Random Delay 開關
 *
 * SAVE 儲存後留在 SETTINGS 頁面；HOME 不儲存直接返回。
 *
 * @version 2.0.0
 */

#include "mode_settings.h"
#include "game_fsm.h"
#include "hal_storage.h"
#include "preset_mgr.h"
#include "mic_engine.h"
#include "web_server.h"
#include <Arduino.h>

// UI 層前向宣告
namespace UIScreen {
  void drawModeHeader(const char* title, const char* badge, uint16_t badgeColor);
  void drawBtnBar(const char* a, const char* b, const char* c);
  void clearGameArea();
  void drawSettingsScreen(const AppSettings& s);
}


// ============================================================
// 畫面佈局常數（與 drawSettingsScreen 保持一致）
// ============================================================
namespace {
  constexpr int16_t Y0      = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 2; ///< 第一列 Y 起點
  constexpr int16_t ROW_H   = 41;   ///< 每列高度
  constexpr int16_t LBL_X   = 4;    ///< 標籤 X
  constexpr int16_t VAL_X   = 110;  ///< 值欄 X
  constexpr int16_t CX      = HW::LCD_W / 2; ///< 水平中心

  constexpr int16_t PRE_ARR_W  = 24;  ///< Preset 箭頭寬度
  constexpr int16_t PRE_LX     = VAL_X;
  constexpr int16_t PRE_RX     = HW::LCD_W - PRE_ARR_W - 4;

  constexpr int16_t SRC_W   = 60;  ///< Source 按鈕寬度
  constexpr int16_t SRC_H   = 22;
  constexpr int16_t SRC_GAP = 4;
  constexpr int16_t SRC_X0  = VAL_X;

  constexpr int16_t THR_BTN_W = 28; ///< Threshold 按鈕寬度
  constexpr int16_t THR_LX    = VAL_X;
  constexpr int16_t THR_RX    = HW::LCD_W - THR_BTN_W - 4;

  constexpr int16_t RND_W   = 60;
  constexpr int16_t RND_X   = VAL_X;
}


// ============================================================
// [SET-impl] ModeBase 介面
// ============================================================

/**
 * @brief 進入 SETTINGS 模式，載入目前生效的設定值
 *
 * 從 PresetMgr::getEffectiveSettings() 取得目前值，
 * 所有調整只修改本地 _edit，不立即套用。
 */
void ModeSettings::onEnter() {
  _edit = PresetMgr::getEffectiveSettings();
  UIScreen::clearGameArea();
  redraw();
  Serial.printf("[ModeSettings] Enter: preset=%u thresh=%d src=%u rand=%u\n",
                _edit.activePreset, _edit.micThresh,
                (uint8_t)_edit.hitSource, _edit.randomDelay);
}

/// @brief 每幀更新（Settings 靜態畫面，無需更新）
void ModeSettings::update() {
  // Settings 畫面靜態顯示，無需每幀更新
}

/// @brief 離開 SETTINGS 模式
void ModeSettings::onExit() {
  Serial.println("[ModeSettings] Exit");
}

/**
 * @brief 按鍵事件處理
 *
 * - BtnA (0)：HOME — 放棄所有變更，返回 Home
 * - BtnC (2)：SAVE — 儲存並留在 SETTINGS 頁面
 *
 * @param btn  按鍵索引（0=A / 1=B / 2=C）
 */
void ModeSettings::onButton(uint8_t btn) {
  if (btn == 0) {
    // BtnA = 取消，不儲存直接返回
    GameFSM::requestSwitch(AppMode::HOME);
  } else if (btn == 2) {
    // BtnC = 儲存並退出
    saveAndExit();
  }
}

/**
 * @brief 觸控事件處理（4 個設定列的觸控熱區）
 *
 * Row 0（Preset）：左側 ◀ / 右側 ▶ 切換槍型，同步載入對應 thresh/src
 * Row 1（Source）：三個按鈕（ESP-NOW / MIC / BOTH）
 * Row 2（Thresh）：左側 − / 右側 + 調整 RMS 門檻（步進 THRESH_STEP）
 * Row 3（Rand Delay）：點擊切換 ON/OFF
 *
 * 所有變更只修改 _edit，需按 SAVE 才會實際套用。
 *
 * @param x  觸控 X 座標
 * @param y  觸控 Y 座標
 */
void ModeSettings::onTouch(int16_t x, int16_t y) {
  Serial.printf("[ModeSettings] touch x=%d y=%d\n", x, y);

  // ── Row 0：PRESET ────────────────────────────────────────────
  // ◀ 熱區：x=80~190（含觸控偏移裕量）
  // ▶ 熱區：x=240~320
  if (y >= Y0 && y <= Y0 + ROW_H) {
    if (x >= 80 && x <= VAL_X + 80) {
      // ◀
      if (_edit.activePreset > 0) _edit.activePreset--;
      else _edit.activePreset = Limits::PRESET_MAX - 1;
      Preset p; PresetMgr::get(_edit.activePreset, p);
      _edit.micThresh = p.micThresh;
      _edit.hitSource = p.hitSource;
      redraw(); return;
    }
    if (x >= HW::LCD_W - 80) {
      // ▶
      _edit.activePreset = (_edit.activePreset + 1) % Limits::PRESET_MAX;
      Preset p; PresetMgr::get(_edit.activePreset, p);
      _edit.micThresh = p.micThresh;
      _edit.hitSource = p.hitSource;
      redraw(); return;
    }
  }

  // ── Row 1：SOURCE 三個按鈕 ──────────────────────────────────
  {
    constexpr int16_t SX = VAL_X, BW = 64, GAP = 6;
    int16_t ry = Y0 + ROW_H;
    if (y >= ry && y <= ry + ROW_H) {
      for (uint8_t i = 0; i < 3; i++) {
        int16_t bx = SX + i * (BW + GAP);
        if (x >= bx && x <= bx + BW) {
          _edit.hitSource = static_cast<HitSource>(i);
          redraw(); return;
        }
      }
    }
  }

  // ── Row 2：THRESH ────────────────────────────────────────────
  // − 熱區：x=80~190（含觸控偏移裕量）
  // + 熱區：x=240~320
  {
    int16_t ry = Y0 + 2 * ROW_H;
    if (y >= ry && y <= ry + ROW_H) {
      if (x >= 80 && x <= VAL_X + 80) {
        // −
        _edit.micThresh -= MicCfg::THRESH_STEP;
        if (_edit.micThresh < MicCfg::THRESH_MIN)
          _edit.micThresh = MicCfg::THRESH_MIN;
        redraw(); return;
      }
      if (x >= HW::LCD_W - 80) {
        // +
        _edit.micThresh += MicCfg::THRESH_STEP;
        if (_edit.micThresh > MicCfg::THRESH_MAX)
          _edit.micThresh = MicCfg::THRESH_MAX;
        redraw(); return;
      }
    }
  }

  // ── Row 3：RAND DELAY toggle ─────────────────────────────────
  {
    int16_t ry = Y0 + 3 * ROW_H;
    if (y >= ry && y <= ry + ROW_H && x >= VAL_X) {
      _edit.randomDelay = !_edit.randomDelay;
      redraw(); return;
    }
  }
}


// ============================================================
// [SET-impl] 私有輔助
// ============================================================

/// @brief 以目前 _edit 重繪 Settings 畫面
void ModeSettings::redraw() {
  UIScreen::drawSettingsScreen(_edit);
}

/**
 * @brief 儲存所有設定並留在 SETTINGS 頁面
 *
 * 儲存流程：
 *  1. PresetMgr::setActive() — 切換 active preset 並套用到 MicEngine
 *  2. PresetMgr::save() — 將 _edit 的 thresh/src 寫入 preset NVS
 *  3. MicEngine::setConfig() — 即時套用 threshold / source
 *  4. PresetMgr::applySettings() — 套用 random delay / par time
 *  5. HalStorage::saveSettings() — 持久化整個 AppSettings
 *  6. WebServer::sendSettings() — 推播 SSE 通知 Web Dashboard
 *  7. redraw() — 重繪畫面確認儲存成功
 */
void ModeSettings::saveAndExit() {
  // 1. 切換 active preset（同時套用到 MicEngine）
  PresetMgr::setActive(_edit.activePreset);

  // 2. 更新 active preset 的 thresh / src（使用者可能調整過）
  Preset p; PresetMgr::get(_edit.activePreset, p);
  p.micThresh = _edit.micThresh;
  p.hitSource = _edit.hitSource;
  PresetMgr::save(_edit.activePreset, p);

  // 3. 套用 source + thresh 到 MicEngine
  MicEngine::setConfig(_edit.micThresh, MicCfg::COOLDOWN_MS, _edit.hitSource);

  // 4. 套用 random delay + par time 到 TimerCore
  PresetMgr::applySettings(_edit);

  // 5. 持久化整個 AppSettings
  HalStorage::saveSettings(_edit);

  Serial.printf("[ModeSettings] Saved: preset=%u thresh=%d src=%u rand=%u\n",
                _edit.activePreset, _edit.micThresh,
                (uint8_t)_edit.hitSource, _edit.randomDelay);

  // 推播 SSE 讓 Web 即時同步
  WebServer::sendSettings(_edit.activePreset, _edit.micThresh,
                          static_cast<uint8_t>(_edit.hitSource));

  // 留在 SETTINGS 頁面，重繪確認儲存成功
  redraw();
}
