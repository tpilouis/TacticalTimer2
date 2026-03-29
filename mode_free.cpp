/**
 * @file    mode_free.cpp
 * @brief   TACTICAL TIMER 2 — Free Shooting 模式實作
 *
 * 自由射擊模式：無發數限制，計時從 beep 開始到最後一槍，
 * 支援 Par Time 設定，結束時顯示成績並儲存 session。
 *
 * @version 2.0.0
 */

#include "mode_free.h"
#include "game_fsm.h"
#include "web_server.h"
#include "hal_storage.h"
#include "preset_mgr.h"
#include <Arduino.h>

// UI 層前向宣告（Phase 5 實作）
namespace UIScreen {
  void drawModeHeader(const char* title, const char* badge, uint16_t badgeColor);
  void drawShotRow(uint8_t hitIdx, uint8_t stationId,
                   unsigned long elapsed, unsigned long split,
                   bool pass = true);
  void drawResultScreen(unsigned long totalMs, uint8_t hitCount,
                        float bestTotal, float bestAvg);
  void clearGameArea();
  void drawBtnBar(const char* a, const char* b, const char* c);
  void drawParSetScreen(uint32_t parMs);
}

/// 模組單例指標（供靜態 applySetPar 存取實例成員）
static ModeFree* s_freeInstance = nullptr;


// ============================================================
// [FREE-impl] ModeBase 介面
// ============================================================

/**
 * @brief 進入 FREE 模式
 *
 * 清除畫面，繪製 STANDBY 標頭，通知 Web 新局開始。
 */
void ModeFree::onEnter() {
  s_freeInstance = this;
  _resultShown = false;
  UIScreen::clearGameArea();
  UIScreen::drawModeHeader("FREE SHOOTING", "STANDBY", 0x5AB0);
  UIScreen::drawBtnBar("STOP", "PAR SET", "START");

  // 通知 Web：進入新局（清除舊紀錄）
  WebServer::sendNewGame();
  WebServer::sendGameState();

  Serial.println("[ModeFree] onEnter");
}

/**
 * @brief 每幀更新（驅動 TimerCore 的各個 process 函式）
 */
void ModeFree::update() {
  // 依序驅動 TimerCore 的各個 process 函式
  TimerCore::processHits(onHit);
  TimerCore::processAudioDone(onReadyDone, onBeepDone, onClearDone);
  TimerCore::processDelay(onDelayExpired);
  TimerCore::processParTime(onParExpired);
}

/**
 * @brief 離開 FREE 模式，若計時中則強制停止
 */
void ModeFree::onExit() {
  s_freeInstance = nullptr;
  // 若仍在計時中，強制停止
  GameState gs = TimerCore::getState();
  if (gs == GameState::GOING || gs == GameState::AREYOUREADY ||
      gs == GameState::BEEPING || gs == GameState::WAITING) {
    stopAudio();
    TimerCore::forceStop();
  }
  Serial.println("[ModeFree] onExit");
}

/**
 * @brief 實體按鍵事件處理
 *
 * - BtnA (0)：STOP（計時中）/ 返回 Home（IDLE/STOP）
 * - BtnB (1)：進入 Par Time 設定畫面（IDLE/STOP 時）
 * - BtnC (2)：START / RESTART
 *
 * @param btn  按鍵索引（0=A / 1=B / 2=C）
 */
void ModeFree::onButton(uint8_t btn) {
  GameState gs = TimerCore::getState();
  Serial.printf("[ModeFree] onButton btn=%u gs=%u\n", btn, (uint8_t)gs);

  switch (btn) {
    case 0:  // BtnA — STOP / 返回 Home
      if (gs == GameState::GOING || gs == GameState::AREYOUREADY ||
          gs == GameState::BEEPING || gs == GameState::WAITING) {
        stopAudio();
        TimerCore::forceStop();
        WebServer::sendGameState();
      } else if (gs == GameState::STOP || gs == GameState::IDLE) {
        GameFSM::requestSwitch(AppMode::HOME);
      } else {
        Serial.printf("[ModeFree] BtnA ignored, gs=%u not STOP/IDLE/GOING\n", (uint8_t)gs);
      }
      break;

    case 1:  // BtnB — Par Time 設定（IDLE/STOP 狀態）
      if (gs == GameState::IDLE || gs == GameState::STOP) {
        enterParSet();
      } else {
        Serial.printf("[ModeFree] BtnB ignored, gs=%u\n", (uint8_t)gs);
      }
      break;

    case 2:  // BtnC — START / RESTART
      if (gs == GameState::IDLE || gs == GameState::STOP) {
        uint8_t gi = TimerCore::startNew();
        playAudio(SND::TT2_READY);
        WebServer::sendNewGame();
        WebServer::sendGameState();
        UIScreen::clearGameArea();
        UIScreen::drawModeHeader("FREE SHOOTING", "READY", 0xFFC0);
        UIScreen::drawBtnBar("STOP", "PAR SET", "RESTART");
        Serial.printf("[ModeFree] Start -> gameIdx=%u\n", gi);
      } else {
        Serial.printf("[ModeFree] BtnC ignored, gs=%u not STOP/IDLE\n", (uint8_t)gs);
      }
      break;

    default: break;
  }
}

/**
 * @brief 觸控事件處理（僅在 Par Set 畫面有效）
 * @param x  觸控 X 座標
 * @param y  觸控 Y 座標
 */
void ModeFree::onTouch(int16_t x, int16_t y) {
  Serial.printf("[ModeFree] onTouch x=%d y=%d parSet=%d\n", x, y, _inParSet);

  if (_inParSet) {
    handleParSetTouch(x, y);
  }
}


// ============================================================
// [FREE-impl] 靜態 callback 實作
// ============================================================

/**
 * @brief 命中事件 callback（由 TimerCore::processHits 呼叫）
 *
 * 更新螢幕 shot row，並透過 SSE 推播命中資料到 Web。
 *
 * @param hitIdx    本局第幾槍（1-based）
 * @param stationId 命中來源（0=MIC, 1+=ESP-NOW station）
 * @param elapsed   從開始到本槍的累積時間（ms）
 * @param split     從上一槍到本槍的分段時間（ms）
 */
void ModeFree::onHit(uint8_t hitIdx, uint8_t stationId,
                     unsigned long elapsed, unsigned long split) {
  // 更新螢幕
  UIScreen::drawShotRow(hitIdx, stationId, elapsed, split);

  // SSE 推播
  SseMsg sm;
  sm.hitIdx    = hitIdx;
  sm.stationId = stationId;
  sm.elapsed   = elapsed;
  sm.split     = split;
  IPC::sendSse(sm);

  Serial.printf("[ModeFree] Hit #%u src=%u T=%lu split=%lu\n",
                hitIdx, stationId, elapsed, split);
}

/// @brief AREYOUREADY 音效播完 → 更新標頭顯示 WAITING
void ModeFree::onReadyDone() {
  // AREYOUREADY 音效播完 → 延遲計時已在 processAudioDone 啟動
  UIScreen::drawModeHeader("FREE SHOOTING", "WAITING", 0xFFC0);
  Serial.println("[ModeFree] ReadyDone -> waiting delay");
}

/// @brief START beep 播完 → 進入 GOING 狀態，更新標頭顯示 GOING
void ModeFree::onBeepDone() {
  // BEEP 播完 → GOING（markStart 已在 processAudioDone 呼叫）
  UIScreen::drawModeHeader("FREE SHOOTING", "GOING", 0x07E0);
  UIScreen::drawBtnBar("STOP", "PAR SET", "RESTART");
  WebServer::sendGameState();
  Serial.println("[ModeFree] BeepDone -> GOING");
}

/**
 * @brief SHOWCLEAR 音效播完 → 顯示成績並儲存 session
 *
 * 附上 active preset 名稱作為 SessionExtra，
 * 呼叫 saveLastSession() 更新最佳紀錄並寫入 SD。
 */
void ModeFree::onClearDone() {
  const GameRecord* rec = TimerCore::getRecord(0);
  unsigned long totalMs = rec ? TimerCore::calcTotalMs(*rec) : 0;
  uint8_t hitCount      = rec ? rec->hit_count : 0;

  // 儲存 session（同時更新 TimerCore 內的最佳紀錄）
  SessionExtra extra;
  const Preset& pr = PresetMgr::getActive();
  extra.presetName = pr.name[0] ? pr.name : nullptr;
  SessionMgr::saveLastSession(AppMode::FREE, [](float t, float a) {
    WebServer::sendBest(t, a);
  }, &extra);

  // 讀取最終最佳值（saveLastSession 已確保更新）
  float bestTotal = SessionMgr::getBestTotal();
  float bestAvg   = SessionMgr::getBestAvg();

  Serial.printf("[ModeFree] ClearDone -> STOP total=%lums hits=%u bestT=%.2f bestA=%.2f\n",
                totalMs, hitCount, bestTotal, bestAvg);

  UIScreen::drawResultScreen(totalMs, hitCount, bestTotal, bestAvg);
  UIScreen::drawBtnBar("HOME", "PAR SET", "RESTART");
  WebServer::sendGameState();
}

/**
 * @brief 延遲計時到期 callback
 *
 * - pending == BEEPING：播放 START beep
 * - pending == SHOWCLEAR：播放 CLEAR 音效（AFTER_CLEAR_DELAY 後）
 *
 * @param pending  即將轉入的 GameState
 */
void ModeFree::onDelayExpired(GameState pending) {
  if (pending == GameState::BEEPING) {
    // 延遲結束 → 播放 beep
    AudioCmd cmd;
    cmd.op = AudioCmd::Op::PLAY;
    strlcpy(cmd.path, SND::TT2_BEEP, sizeof(cmd.path));
    IPC::sendAudioCmd(cmd);
    UIScreen::drawModeHeader("FREE SHOOTING", "BEEP!", 0x07E0);
    Serial.println("[ModeFree] DelayExpired -> play BEEP");
  } else if (pending == GameState::SHOWCLEAR) {
    // AFTER_CLEAR_DELAY 結束 → 播放 Clear 音效
    AudioCmd cmd;
    cmd.op = AudioCmd::Op::PLAY;
    strlcpy(cmd.path, SND::TT2_CLEAR, sizeof(cmd.path));
    IPC::sendAudioCmd(cmd);
    UIScreen::drawModeHeader("FREE SHOOTING", "SHOW CLEAR", 0x8080);
    Serial.println("[ModeFree] DelayExpired -> play CLEAR");
  }
}

/// @brief Par Time 到期 → 播放第二聲 beep 提醒
void ModeFree::onParExpired() {
  // Par time 到期 → 播放第二聲 beep 提醒
  AudioCmd cmd;
  cmd.op = AudioCmd::Op::PLAY;
  strlcpy(cmd.path, SND::TT2_BEEP, sizeof(cmd.path));
  IPC::sendAudioCmd(cmd);
  Serial.println("[ModeFree] Par time expired -> play BEEP");
}

// ============================================================
// [FREE-impl] Par Time 設定畫面
// ============================================================

/// @brief 進入 Par Time 設定畫面，讀取目前 par time 並繪製
void ModeFree::enterParSet() {
  _inParSet = true;
  // 讀取目前 par time
  AppSettings eff = PresetMgr::getEffectiveSettings();
  _parSetMs = eff.parTimeMs;
  UIScreen::drawParSetScreen(_parSetMs);
  Serial.printf("[ModeFree] enterParSet parMs=%u\n", _parSetMs);
}

/**
 * @brief 離開 Par Time 設定畫面
 *
 * @param save  true = 儲存並套用新值；false = 取消，恢復原值
 */
/**
 * @brief Web 設定 Par Time 後同步到 Core2（在 loop Task 執行）
 *
 * 若目前在 PAR SET 畫面，同步更新 _parSetMs 並重繪；
 * 否則只更新 _parSetMs，下次進入 PAR SET 時會載入正確值。
 *
 * @param ms  新的 Par Time（ms），由 GameFSM::processWebCmd 呼叫
 */
void ModeFree::applySetPar(uint32_t ms) {
  if (!s_freeInstance) return;
  s_freeInstance->_parSetMs = ms;
  // 若目前正在 PAR SET 畫面，立即重繪讓 Core2 螢幕同步
  if (s_freeInstance->_inParSet) {
    UIScreen::drawParSetScreen(ms);
  }
  Serial.printf("[ModeFree] applySetPar: %u ms\n", ms);
}

void ModeFree::exitParSet(bool save) {
  _inParSet = false;
  if (save) {
    // 套用到 TimerCore（即時生效）
    TimerCore::setParTime(_parSetMs);
    // 持久化到 NVS
    AppSettings eff = PresetMgr::getEffectiveSettings();
    eff.parTimeMs = _parSetMs;
    HalStorage::saveSettings(eff);
    // 同步更新 PresetMgr 記憶體快照（確保 s_settings 也更新）
    PresetMgr::applySettings(eff);
    // 推播 SSE 讓 Web 即時同步
    WebServer::sendParTime(_parSetMs, TimerCore::isRandomDelay());
    Serial.printf("[ModeFree] Par time saved: %u ms\n", _parSetMs);
  } else {
    Serial.println("[ModeFree] Par time cancelled");
  }
  // 恢復 Free 模式畫面
  GameState gs = TimerCore::getState();
  UIScreen::clearGameArea();
  if (gs == GameState::STOP) {
    // 結果畫面
    const GameRecord* rec = TimerCore::getRecord(0);
    unsigned long totalMs = rec ? TimerCore::calcTotalMs(*rec) : 0;
    uint8_t hitCount      = rec ? rec->hit_count : 0;
    float bestTotal = SessionMgr::getBestTotal();
    float bestAvg   = SessionMgr::getBestAvg();
    UIScreen::drawModeHeader("FREE SHOOTING", "STOP", 0x5AB0);
    UIScreen::drawResultScreen(totalMs, hitCount, bestTotal, bestAvg);
    UIScreen::drawBtnBar("HOME", "PAR SET", "RESTART");
  } else {
    UIScreen::drawModeHeader("FREE SHOOTING", "STANDBY", 0x5AB0);
    UIScreen::drawBtnBar("STOP", "PAR SET", "START");
  }
}

/**
 * @brief Par Time 設定畫面的觸控事件處理
 *
 * 處理 −500ms / +500ms 調整按鈕，以及 CANCEL / DONE 動作按鈕。
 *
 * @param x  觸控 X 座標
 * @param y  觸控 Y 座標
 */
void ModeFree::handleParSetTouch(int16_t x, int16_t y) {
  // 與 drawParSetScreen 座標一致
  constexpr int16_t Y0     = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 8;
  constexpr int16_t BTN_W  = 50;
  constexpr int16_t BTN_H  = 36;
  constexpr int16_t BTN_Y  = Y0 + 34;
  constexpr int16_t CX     = HW::LCD_W / 2;
  constexpr int16_t BTN_LX = CX - 80 - BTN_W;   // 30
  constexpr int16_t BTN_RX = CX + 80;            // 240
  constexpr int16_t ACT_W  = 100;
  constexpr int16_t ACT_H  = 28;
  constexpr int16_t ACT_Y  = Y0 + 104;
  constexpr int16_t ACT_LX = CX - 110;           // 50
  constexpr int16_t ACT_RX = CX + 10;            // 170
  constexpr uint32_t STEP  = Timing::PAR_TIME_STEP_MS * 5; // 500ms

  // − 按鈕
  if (x >= BTN_LX && x <= BTN_LX + BTN_W &&
      y >= BTN_Y  && y <= BTN_Y  + BTN_H) {
    if (_parSetMs >= STEP) _parSetMs -= STEP;
    else                   _parSetMs  = 0;
    UIScreen::drawParSetScreen(_parSetMs);
    Serial.printf("[ModeFree] Par - : %u ms\n", _parSetMs);
    return;
  }

  // + 按鈕
  if (x >= BTN_RX && x <= BTN_RX + BTN_W &&
      y >= BTN_Y  && y <= BTN_Y  + BTN_H) {
    if (_parSetMs + STEP <= Timing::PAR_TIME_MAX_MS)
      _parSetMs += STEP;
    UIScreen::drawParSetScreen(_parSetMs);
    Serial.printf("[ModeFree] Par + : %u ms\n", _parSetMs);
    return;
  }

  // CANCEL 按鈕
  if (x >= ACT_LX && x <= ACT_LX + ACT_W &&
      y >= ACT_Y  && y <= ACT_Y  + ACT_H) {
    exitParSet(false);
    return;
  }

  // DONE 按鈕
  if (x >= ACT_RX && x <= ACT_RX + ACT_W &&
      y >= ACT_Y  && y <= ACT_Y  + ACT_H) {
    exitParSet(true);
    return;
  }
}
