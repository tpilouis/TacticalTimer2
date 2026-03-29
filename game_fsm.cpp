/**
 * @file    game_fsm.cpp
 * @brief   TACTICAL TIMER 2 — 主狀態機實作
 *
 * 實作 AppMode 切換、觸控事件分派、Web 指令路由，
 * 以及 Home 畫面的 6-card grid 觸控處理。
 *
 * @version 2.0.0
 */

#include "game_fsm.h"
#include "web_server.h"
#include <M5Core2.h>

// 前向宣告所有 Mode 類別（在各自 .h 中定義）
#include "mode_free.h"
#include "mode_drill.h"
#include "mode_dryfire.h"
#include "mode_spy.h"
#include "mode_ro.h"
#include "mode_history.h"
#include "mode_settings.h"


// ============================================================
// [FSM0] 模組內部狀態
// ============================================================
namespace {

  // 所有模式實例（靜態分配，避免 heap 碎片）
  static ModeFree     s_modeFree;
  static ModeDrill    s_modeDrill;
  static ModeDryFire  s_modeDryFire;
  static ModeSpy      s_modeSpy;
  static ModeRO       s_modeRO;
  static ModeHistory  s_modeHistory;
  static ModeSettings s_modeSettings;

  // 模式查找表（AppMode → ModeBase*）
  ModeBase* s_modeTable[9] = {
    nullptr,           // [0] NONE
    nullptr,           // [1] HOME  (Home 由 FSM 直接處理，無 ModeBase)
    &s_modeFree,       // [2] FREE
    &s_modeDrill,      // [3] DRILL
    &s_modeDryFire,    // [4] DRY_FIRE
    &s_modeSpy,        // [5] SPY
    &s_modeRO,         // [6] RO
    &s_modeHistory,    // [7] HISTORY
    &s_modeSettings,   // [8] SETTINGS
  };

  ModeBase* s_current     = nullptr;
  AppMode   s_currentMode = AppMode::NONE;
  AppMode   s_pendingMode = AppMode::NONE;  ///< requestSwitch 的目標
  bool      s_switchPending = false;

  unsigned long s_lastStatusUpdate = 0;

} // anonymous namespace


// ============================================================
// [FSM-internal] 狀態列 / IP 列更新（前向宣告 UI 函式）
// ============================================================
namespace UIScreen {
  void drawStatusBar();
  void drawIPBar();
  void drawHomeScreen();
}


// ============================================================
// [F2-impl] GameFSM
// ============================================================

/**
 * @brief 初始化 GameFSM
 *
 * 初始化 TimerCore，設定初始模式為 HOME，並繪製 Home 畫面。
 * 必須在所有 Mode 物件建立後呼叫。
 */
void GameFSM::init() {
  // 計時核心
  TimerCore::init();

  // 初始模式：Home
  s_currentMode   = AppMode::HOME;
  s_current       = nullptr;  // Home 無 ModeBase
  s_switchPending = false;
  s_pendingMode   = AppMode::NONE;

  // 畫 Home 畫面
  UIScreen::drawHomeScreen();

  Serial.println("[GameFSM] Initialized, mode=HOME");
}

/**
 * @brief GameFSM 主迴圈（每幀呼叫）
 *
 * 負責：
 *  1. M5.update()（按鍵 / 觸控狀態更新）
 *  2. 觸控事件 debounce 並派發到對應 mode 的 onTouch() / onButton()
 *  3. 呼叫目前 mode 的 update()
 *  4. 從 uiCmdQueue 取出 Web 指令並路由到對應 mode
 *  5. 執行 requestSwitch() 排定的模式切換
 *  6. 定時更新 Status bar（每 2 秒）
 */
void GameFSM::update() {
  M5.update();

  // ── 觸控事件（debounce：手指按下只觸發一次）────────────────
  // 問題根源：ispressed() 在長按期間每幀都回傳 true，
  // 導致 onTouch() 被連續呼叫數百次，造成重入和觸控鎖死。
  // 解法：用靜態旗標，手指按下時只處理第一幀，
  //       手指離開後才允許下次觸控。
  static bool s_touchConsumed = false;

  if (M5.Touch.ispressed()) {
    if (!s_touchConsumed) {
      s_touchConsumed = true;
      TouchPoint tp = M5.Touch.getPressPoint();
      int16_t tx = static_cast<int16_t>(tp.x);
      int16_t ty = static_cast<int16_t>(tp.y);

      Serial.printf("[GameFSM] touch x=%d y=%d\n", tx, ty);

      // ── 過濾無效觸控點（FT6336 無觸控時回傳 -1）────────────
      if (tx < 0 || ty < 0) {
        s_touchConsumed = false;  // 視為無效，允許下次觸控
      }
      // ── 底部 Btn Bar（Y >= BTN_Y）→ 直接轉 onButton ────────
      // Core2 的 BtnA/B/C.wasPressed() 與 onTouch() 共用同一觸控事件，
      // s_touchConsumed 會使 wasPressed() 永遠得不到事件，
      // 因此統一在這裡用座標判斷來派發按鍵事件。
      else if (ty >= Layout::BTN_Y) {
        uint8_t btn = (tx < HW::LCD_W / 3) ? 0 :
                      (tx < HW::LCD_W * 2 / 3) ? 1 : 2;
        if (s_current) {
          s_current->onButton(btn);
        } else if (s_currentMode == AppMode::HOME && btn == 1) {
          requestSwitch(AppMode::SETTINGS);
        }
      }
      // ── 遊戲區 / Home 觸控 ────────────────────────────────
      else if (s_current) {
        s_current->onTouch(tx, ty);
      } else if (s_currentMode == AppMode::HOME) {
        GameFSM::handleHomeTap(tx, ty);
      }
    }
  } else {
    // 手指離開 → 重置，允許下次觸控
    s_touchConsumed = false;
  }

  // ── 實體按鍵事件（BtnA/B/C）─────────────────────────────
  // 注意：已改用座標判斷派發，wasPressed() 備援已停用。
  // 保留程式碼供參考，但不執行，避免與座標判斷雙重觸發。
  // if (M5.BtnA.wasPressed() && s_current) s_current->onButton(0);
  // if (M5.BtnB.wasPressed()) { ... }
  // if (M5.BtnC.wasPressed() && s_current) s_current->onButton(2);

  // ── 模式 update ───────────────────────────────────────────
  if (s_current) {
    s_current->update();
  }

  // ── Web 指令（全域，任何模式都響應）─────────────────────
  TimerCore::processWebCmd(
    []() {  // onStart
      if (s_currentMode == AppMode::DRY_FIRE) {
        if (!ModeDryFire::isRunning()) ModeDryFire::doStart();
      } else if (s_currentMode == AppMode::SPY) {
        // 只在非 listening 狀態才 start（避免誤 toggle）
        if (s_current) {
          ModeSpy* spy = static_cast<ModeSpy*>(s_current);
          if (!spy->isListening()) s_current->onButton(2);
        }
      } else {
        GameState gs = TimerCore::getState();
        if (gs == GameState::IDLE || gs == GameState::STOP) {
          if (s_currentMode == AppMode::FREE) {
            if (s_current) s_current->onButton(2);
          } else if (s_currentMode == AppMode::DRILL) {
            ModeDrill::webStart();
          } else if (s_currentMode == AppMode::RO) {
            if (s_current) s_current->onButton(2);
          }
        }
      }
    },
    []() {  // onStop
      if (s_currentMode == AppMode::DRY_FIRE) {
        ModeDryFire::doStop();
      } else if (s_currentMode == AppMode::SPY) {
        // 只在 listening 狀態才 stop（避免跳 HOME）
        if (s_current) {
          ModeSpy* spy = static_cast<ModeSpy*>(s_current);
          if (spy->isListening()) s_current->onButton(0);
        }
      } else if (s_currentMode == AppMode::FREE || s_currentMode == AppMode::DRILL ||
                 s_currentMode == AppMode::RO) {
        if (s_current) s_current->onButton(0);
      }
    },
    [](uint32_t ms) {  // onSetBeat
      if (s_currentMode == AppMode::DRY_FIRE) {
        ModeDryFire::applySetBeat(ms);
      }
    },
    []() {  // onSpyClear
      if (s_currentMode == AppMode::SPY) {
        if (s_current) s_current->onButton(1);  // BtnB = CLEAR
      }
    },
    [](uint32_t ms) {  // onSetPar
      if (s_currentMode == AppMode::FREE) {
        ModeFree::applySetPar(ms);
      }
    }
  );

  // ── 執行模式切換（在 update 之後，避免 re-entrancy）──────
  if (s_switchPending) {
    s_switchPending = false;
    switchTo(s_pendingMode);
  }

  // ── 定時更新 Status bar（每 2s）──────────────────────────
  if (millis() - s_lastStatusUpdate >= Timing::INFO_INTERVAL_MS) {
    s_lastStatusUpdate = millis();
    UIScreen::drawStatusBar();
  }
}

/**
 * @brief 排定模式切換（在當前幀結束後執行）
 *
 * 設置 s_pendingMode 和 s_switchPending 旗標，
 * 實際切換在 update() 末尾的 switchTo() 中執行，
 * 避免在 mode callback 內部直接切換造成 re-entrancy 問題。
 *
 * @param mode  目標 AppMode
 */
void GameFSM::requestSwitch(AppMode mode) {
  s_pendingMode   = mode;
  s_switchPending = true;
}

/**
 * @brief 立即執行模式切換（不可在 mode callback 內直接呼叫）
 *
 * 流程：
 *  1. 呼叫目前 mode 的 onExit()
 *  2. 更新 s_currentMode
 *  3. HOME 模式特殊處理（無 ModeBase，直接繪製 Home 畫面）
 *  4. 查找 s_modeTable，呼叫新 mode 的 onEnter()
 *  5. 推播 SSE 通知 Web Dashboard 模式已切換
 *
 * @param mode  目標 AppMode
 */
void GameFSM::switchTo(AppMode mode) {
  if (mode == s_currentMode) return;

  Serial.printf("[GameFSM] Switch: %d -> %d\n",
                (int)s_currentMode, (int)mode);

  // 離開目前模式
  if (s_current) {
    s_current->onExit();
    s_current = nullptr;
  }

  s_currentMode = mode;

  // 特殊處理：HOME 和 SETTINGS 無 ModeBase
  if (mode == AppMode::HOME) {
    UIScreen::drawHomeScreen();
    return;
  }

  // 取得對應 ModeBase 並進入
  uint8_t idx = static_cast<uint8_t>(mode);
  if (idx >= 9 || !s_modeTable[idx]) {
    Serial.printf("[GameFSM] No ModeBase for AppMode=%d, back to HOME\n", idx);
    switchTo(AppMode::HOME);
    return;
  }

  s_current = s_modeTable[idx];
  s_current->onEnter();
  WebServer::sendMode(mode);  // 通知 Web Dashboard 模式已切換
}

/// @brief 取得目前 AppMode
/// @return 目前模式（AppMode::HOME / FREE / DRILL / ...）
AppMode         GameFSM::currentMode() { return s_currentMode; }

/// @brief 取得目前 ModeBase 指標（HOME 模式回傳 nullptr）
const ModeBase* GameFSM::current()     { return s_current; }


// ============================================================
// [FSM-internal] Home 畫面觸控處理
// ============================================================

/**
 * @brief 處理 Home 畫面的 6-card grid 觸控事件
 *
 * 將觸控座標映射到 3×2 card grid，計算欄列索引後
 * 查表 cardMode[][] 取得目標 AppMode，呼叫 switchTo()。
 *
 * Grid 佈局（320×184px 遊戲區）：
 * @code
 *  ┌──────┬──────┬──────┐
 *  │ FREE │DRILL │ DRY  │  row 0
 *  ├──────┼──────┼──────┤
 *  │ SPY  │  RO  │HIST  │  row 1
 *  └──────┴──────┴──────┘
 * @endcode
 *
 * @param x  觸控 X 座標（LCD 像素）
 * @param y  觸控 Y 座標（LCD 像素）
 */
void GameFSM::handleHomeTap(int16_t x, int16_t y) {
  // 6-card grid：
  // Y 範圍 = GAME_AREA_Y 到 IP_BAR_Y（20 ~ 204），高度 184px
  // 每欄寬 = (320 - 2*PAD - 2*GAP) / 3 ≈ 102px
  // 每列高 = (184 - 2*PAD - 1*GAP) / 2 ≈ 88px

  constexpr int16_t GY = Layout::GAME_AREA_Y;
  constexpr int16_t GH = Layout::GAME_AREA_H;
  constexpr int16_t P  = Layout::CARD_PAD;
  constexpr int16_t G  = Layout::CARD_GAP;
  constexpr int16_t CW = (HW::LCD_W - 2*P - 2*G) / 3;
  constexpr int16_t CH = (GH - 2*P - 1*G) / 2;

  // 超出範圍守衛
  if (x < P || x > HW::LCD_W - P) return;
  if (y < GY + P || y > GY + GH - P) return;

  uint8_t col = (x - P) / (CW + G);
  uint8_t row = (y - GY - P) / (CH + G);

  if (col >= 3 || row >= 2) return;

  // 卡片索引（row * 3 + col）→ AppMode
  static const AppMode cardMode[2][3] = {
    { AppMode::FREE,    AppMode::DRILL,   AppMode::DRY_FIRE },
    { AppMode::SPY,     AppMode::RO,      AppMode::HISTORY  },
  };

  AppMode target = cardMode[row][col];
  Serial.printf("[GameFSM] Home tap: col=%u row=%u -> mode=%d\n",
                col, row, (int)target);
  switchTo(target);
}
