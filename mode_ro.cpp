/**
 * @file    mode_ro.cpp
 * @brief   TACTICAL TIMER 2 — RO（Range Officer）模式實作
 *
 * RO 模式專為射擊裁判設計：
 *  - 第一槍自動記錄為「Draw Time（拔槍時間）」
 *  - 支援隨機/固定延遲切換
 *  - 結束時儲存 session（含 drawTimeMs 和 presetName）
 *
 * @version 2.0.0
 */

#include "mode_ro.h"
#include "ui_screen.h"
#include "game_fsm.h"
#include "hal_storage.h"
#include "preset_mgr.h"
#include <M5Core2.h>
#include <Arduino.h>

/// @cond INTERNAL
/// 模組單例指標（供靜態 callback 存取實例成員）
static ModeRO* s_roInstance = nullptr;
/// @endcond



/**
 * @brief 進入 RO 模式，重置 draw time 並設定隨機延遲
 */
void ModeRO::onEnter() {
  s_roInstance    = this;
  _drawTimeMs     = 0;
  _drawRecorded   = false;
  // 確保狀態為 IDLE，重設分頁（避免帶入上一局狀態）
  TimerCore::abortSilent();
  UIScreen::resetShotPage();
  TimerCore::setRandomDelay(true);  // RO 預設隨機延遲
  drawScreen("STANDBY", 0x5AB0);
  Serial.println("[ModeRO] onEnter");
}

/// @brief 每幀更新（驅動 TimerCore processHits / processAudioDone / processDelay）
void ModeRO::update() {
  TimerCore::processHits(onHit);
  TimerCore::processAudioDone(onReadyDone, onBeepDone, onClearDone);
  TimerCore::processDelay(onDelayExpired);
}

/// @brief 離開 RO 模式，清除單例指標，若計時中則強制停止
void ModeRO::onExit() {
  s_roInstance = nullptr;
  GameState gs = TimerCore::getState();
  if (gs == GameState::GOING || gs == GameState::AREYOUREADY ||
      gs == GameState::BEEPING || gs == GameState::WAITING) {
    stopAudio();
    TimerCore::forceStop();
  }
  Serial.println("[ModeRO] onExit");
}

/**
 * @brief 按鍵事件處理
 *
 * - BtnA (0)：停止（GOING）/ 返回 Home（IDLE/STOP）
 * - BtnB (1)：切換隨機 / 固定延遲
 * - BtnC (2)：START / RESTART
 *
 * @param btn  按鍵索引（0=A / 1=B / 2=C）
 */
void ModeRO::onButton(uint8_t btn) {
  GameState gs = TimerCore::getState();

  switch (btn) {
    case 0:  // BtnA — 停止 / 上一頁（STOP）/ Home（IDLE）
      if (gs == GameState::GOING) {
        stopAudio();
        TimerCore::forceStop();
        WebServer::sendGameState();
      } else if (gs == GameState::STOP && UIScreen::shotPageHasPrev()) {
        UIScreen::shotPagePrev();
        redrawShotPage();
      } else if (gs == GameState::IDLE || gs == GameState::STOP) {
        GameFSM::requestSwitch(AppMode::HOME);
      }
      break;
    case 1:  // BtnB — 切換隨機/固定延遲
      TimerCore::setRandomDelay(!TimerCore::isRandomDelay());
      drawScreen("STANDBY", 0x5AB0);
      break;
    case 2:  // BtnC — 下一頁（STOP）/ START / RESTART
      if (gs == GameState::STOP && UIScreen::shotPageHasNext()) {
        UIScreen::shotPageNext();
        redrawShotPage();
      } else if (gs == GameState::IDLE || gs == GameState::STOP) {
        _drawTimeMs   = 0;
        _drawRecorded = false;
        AppSettings eff = PresetMgr::getEffectiveSettings();
        TimerCore::setMaxShots(eff.maxShots);
        UIScreen::resetShotPage();  // 新局重設分頁
        TimerCore::startNew();
        playAudio(SND::TT2_READY);
        WebServer::sendNewGame();
        WebServer::sendGameState();
        UIScreen::clearGameArea();
        drawScreen("READY", 0xFFC0);
        UIScreen::drawBtnBar("STOP", "RAND/FIXED", "RESTART");
      }
      break;
    default: break;
  }
}

/**
 * @brief 觸控事件處理（HOME 區域觸控返回 Home）
 * @param x  觸控 X 座標
 * @param y  觸控 Y 座標
 */
void ModeRO::onTouch(int16_t x, int16_t y) {
  if (x >= Layout::HOME_BTN_X && x <= Layout::HOME_BTN_X + Layout::HOME_BTN_W &&
      y >= Layout::HOME_BTN_Y && y <= Layout::HOME_BTN_Y + Layout::HOME_BTN_H) {
    GameFSM::requestSwitch(AppMode::HOME);
  }
}

/**
 * @brief 完整重繪 RO 模式畫面（header + draw time + btn bar）
 * @param badge       標頭 badge 文字（如 "STANDBY"、"GOING"）
 * @param badgeColor  badge 顏色（RGB565）
 */
void ModeRO::drawScreen(const char* badge, uint16_t badgeColor) {
  UIScreen::clearGameArea();
  UIScreen::drawModeHeader("RO MODE", badge, badgeColor);
  bool rd = TimerCore::isRandomDelay();
  const GameRecord* rec = TimerCore::getRecord(0);
  unsigned long totalMs = rec ? TimerCore::calcTotalMs(*rec) : 0;
  UIScreen::drawROScreen(_drawTimeMs,
                         rec ? rec->hit_count : 0,
                         totalMs);
  UIScreen::drawBtnBar("STOP", rd ? "RANDOM" : "FIXED", "START");
  // HOME overlay 移除：BtnA 在 STOP 狀態按第二次即返回 HOME，不需要額外按鈕
}


/**
 * @brief 局部更新 UI（僅刷新 header 和 draw time，保留 shot rows）
 *
 * 避免命中時全畫面重繪造成閃爍。
 *
 * @param badge       標頭 badge 文字
 * @param badgeColor  badge 顏色（RGB565）
 * @param btnA        BtnA 標籤
 * @param btnB        BtnB 標籤（nullptr = 顯示 RANDOM/FIXED 狀態）
 * @param btnC        BtnC 標籤
 */
void ModeRO::roUpdateUI(const char* badge, uint16_t badgeColor,
                        const char* btnA, const char* btnB, const char* btnC) {
  if (!s_roInstance) return;
  // drawModeHeader 已包含完整清除邏輯，不需要額外 fillRect
  UIScreen::drawModeHeader("RO MODE", badge, badgeColor);
  const GameRecord* rec = TimerCore::getRecord(0);
  unsigned long totalMs = rec ? TimerCore::calcTotalMs(*rec) : 0;
  UIScreen::drawROScreen(s_roInstance->_drawTimeMs,
                         rec ? rec->hit_count : 0,
                         totalMs);
  bool rd = TimerCore::isRandomDelay();
  UIScreen::drawBtnBar(btnA, btnB ? btnB : (rd ? "RANDOM" : "FIXED"), btnC);
}

// ── Static callbacks ──────────────────────────────────────────

/**
 * @brief 命中事件 callback
 *
 * 第一槍自動記錄為 Draw Time（_drawTimeMs）。
 * 之後每槍更新 shot row 並推播 SSE。
 *
 * @param hitIdx    本局第幾槍（1-based）
 * @param stationId 命中來源（0=MIC, 1+=ESP-NOW）
 * @param elapsed   從開始到本槍的累積時間（ms）
 * @param split     從上一槍到本槍的分段時間（ms）
 */
void ModeRO::onHit(uint32_t hitIdx, uint8_t stationId,
                   unsigned long elapsed, unsigned long split) {
  if (!s_roInstance) return;

  // 第一槍 = Draw Time，記錄後更新底部顯示
  if (hitIdx == 1 && !s_roInstance->_drawRecorded) {
    s_roInstance->_drawTimeMs   = elapsed;
    s_roInstance->_drawRecorded = true;
    Serial.printf("[ModeRO] Draw time: %lums\n", elapsed);
  }

  // 更新底部 Draw Time（不重繪整個畫面）
  const GameRecord* rec = TimerCore::getRecord(0);
  unsigned long totalMs = rec ? TimerCore::calcTotalMs(*rec) : 0;
  UIScreen::drawROScreen(s_roInstance->_drawTimeMs,
                         rec ? rec->hit_count : 0,
                         totalMs);

  UIScreen::drawShotRow(hitIdx, stationId, elapsed, split);

  SseMsg sm = { hitIdx, stationId, elapsed, split };
  IPC::sendSse(sm);
}

/// @brief AREYOUREADY 音效播完 → 標頭顯示 WAITING
void ModeRO::onReadyDone() {
  roUpdateUI("WAITING", 0xFFC0, "STOP", nullptr, "RESTART");
}

/// @brief START beep 播完 → 標頭顯示 FIRE!，通知 Web GameState
void ModeRO::onBeepDone() {
  roUpdateUI("FIRE!", 0xF800, "STOP", nullptr, "RESTART");
  WebServer::sendGameState();
}

/**
 * @brief SHOWCLEAR 音效播完 → 儲存 session（含 drawTimeMs）並顯示 STOP
 *
 * 將 _drawTimeMs 和 active preset 名稱打包為 SessionExtra，
 * 呼叫 saveLastSession() 寫入 SD。
 */
void ModeRO::redrawShotPage() {
  const GameRecord* rec = TimerCore::getRecord(0);
  if (!rec) return;
  UIScreen::clearGameArea();
  drawScreen("RESULT", 0x5AB0);
  uint32_t total    = TimerCore::getTotalShots();
  uint32_t pageBase = UIScreen::getPageBase();
  uint32_t pageEnd  = pageBase + UIScreen::getRowsPerPage();
  for (uint32_t i = pageBase; i < pageEnd && i <= total; i++) {
    uint32_t slot = (i <= Limits::HIT_MAX) ? (i - 1) : ((i - 1) % (Limits::HIT_MAX - 1) + 1);
    if (slot >= Limits::HIT_MAX) break;
    unsigned long elapsed = TimerCore::calcElapsed(*rec, (uint8_t)(slot + 1));
    unsigned long split   = TimerCore::calcSplit(*rec, (uint8_t)(slot + 1));
    UIScreen::drawShotRowAt(i, pageBase, rec->hits[slot].station_id, elapsed, split, true);
  }
  const char* lblA = UIScreen::shotPageHasPrev() ? "< PREV" : "HOME";
  const char* lblC = UIScreen::shotPageHasNext() ? "NEXT >" : "RESTART";
  UIScreen::drawBtnBar(lblA, "RAND/FIXED", lblC);
}

void ModeRO::onClearDone() {
  SessionExtra extra;
  extra.drawTimeMs = s_roInstance ? s_roInstance->_drawTimeMs : 0;
  const Preset& pr = PresetMgr::getActive();
  extra.presetName = pr.name[0] ? pr.name : nullptr;
  SessionMgr::saveLastSession(AppMode::RO, [](float t, float a) {
    WebServer::sendBest(t, a);
  }, &extra);
  roUpdateUI("STOP", 0x5AB0, "HOME", nullptr, "RESTART");
  WebServer::sendGameState();
}

/**
 * @brief 延遲計時到期 callback
 *
 * - pending == BEEPING：播放 START beep
 * - pending == SHOWCLEAR：播放 CLEAR 音效
 *
 * @param pending  即將轉入的 GameState
 */
void ModeRO::onDelayExpired(GameState pending) {
  if (pending == GameState::BEEPING) {
    AudioCmd cmd; cmd.op = AudioCmd::Op::PLAY;
    strlcpy(cmd.path, SND::TT2_BEEP, sizeof(cmd.path));
    IPC::sendAudioCmd(cmd);
    roUpdateUI("FIRE!", 0xF800, "STOP", nullptr, "RESTART");
  } else if (pending == GameState::SHOWCLEAR) {
    AudioCmd cmd; cmd.op = AudioCmd::Op::PLAY;
    strlcpy(cmd.path, SND::TT2_CLEAR, sizeof(cmd.path));
    IPC::sendAudioCmd(cmd);
    roUpdateUI("SHOW CLEAR", 0x8080, "STOP", nullptr, "RESTART");
    Serial.println("[ModeRO] DelayExpired -> play CLEAR");
  }
}
