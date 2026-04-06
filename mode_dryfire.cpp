/**
 * @file    mode_dryfire.cpp
 * @brief   TACTICAL TIMER 2 — Dry Fire 節拍訓練模式實作
 *
 * 以固定間隔播放 tick 音效，協助射手進行扣板節奏訓練。
 * 不使用 Mic / TimerCore，beat interval 可即時調整（500–5000ms）。
 *
 * @version 2.0.0
 */

#include "mode_dryfire.h"
#include "game_fsm.h"
#include "web_server.h"
#include "hal_storage.h"
#include <Arduino.h>

/// @cond INTERNAL
/// 模組單例指標（供靜態 public 函式存取實例成員）
static ModeDryFire* s_dfInstance = nullptr;
/// @endcond

namespace UIScreen {
  void drawModeHeader(const char*, const char*, uint16_t);
  void clearGameArea();
  void drawBtnBar(const char*, const char*, const char*);
  void drawDryFireScreen(uint32_t beatMs, uint32_t beatCount,
                         unsigned long elapsedMs, bool running);
}


/**
 * @brief 進入 DRY FIRE 模式，從 NVS 載入上次 beatMs 並通知 Web
 */
void ModeDryFire::onEnter() {
  s_dfInstance  = this;
  _running      = false;
  _beatCount    = 0;
  _lastBeatAt   = 0;
  _sessionStart = 0;

  // 從 AppSettings 載入上次儲存的 beatMs
  AppSettings cfg = PresetMgr::getEffectiveSettings();
  if (cfg.dryBeatMs >= Timing::DRY_BEAT_MIN_MS &&
      cfg.dryBeatMs <= Timing::DRY_BEAT_MAX_MS) {
    _beatMs = cfg.dryBeatMs;
  }

  // 確保麥克風停用（Dry Fire 不需要 Mic）
  MicEngine::setSpyMode(false);

  drawScreen();
  // 通知 Web 目前的 beatMs（讓 Web 顯示與 Core2 一致）
  WebServer::sendDryBeatMs(_beatMs);
  Serial.printf("[ModeDryFire] onEnter beatMs=%u\n", _beatMs);
}

/// @brief 每幀更新：若 running 則呼叫 tick() 驅動節拍
void ModeDryFire::update() {
  if (_running) tick();
}

/// @brief 離開 DRY FIRE 模式，若正在執行則先停止 session
void ModeDryFire::onExit() {
  if (_running) stopSession();
  s_dfInstance = nullptr;
  Serial.println("[ModeDryFire] onExit");
}

/**
 * @brief 儲存目前 _beatMs 到 NVS 並推播 SSE 給 Web
 *
 * 寫入 AppSettings.dryBeatMs 並呼叫 PresetMgr::applySettings()
 * 確保即時生效。
 */
void ModeDryFire::saveBeatMs() {
  AppSettings s = PresetMgr::getEffectiveSettings();
  s.dryBeatMs = _beatMs;
  HalStorage::saveSettings(s);
  PresetMgr::applySettings(s);
  WebServer::sendDryBeatMs(_beatMs);
}

/**
 * @brief 取得目前 beat interval（ms）
 * @return _beatMs（無實例時回傳 DRY_BEAT_DEF_MS）
 */
uint32_t ModeDryFire::getBeatMs() {
  return s_dfInstance ? s_dfInstance->_beatMs : Timing::DRY_BEAT_DEF_MS;
}

/**
 * @brief 透過 IPC queue 設定 beat interval（執行緒安全）
 *
 * 由 Web 呼叫，傳送 DRY_SET_BEAT 指令到 uiCmdQueue，
 * 由 loop() Task 中的 applySetBeat() 實際執行。
 *
 * @param ms  目標 beat interval（會被 clamp 至 DRY_BEAT_MIN–MAX）
 */
void ModeDryFire::setBeatMs(uint32_t ms) {
  ms = constrain(ms, Timing::DRY_BEAT_MIN_MS, Timing::DRY_BEAT_MAX_MS);
  UiCmdMsg m; m.cmd = WebCmd::DRY_SET_BEAT; m.beatMs = ms;
  IPC::sendUiCmd(m);
}

/// @brief Web 請求開始（透過 IPC queue，執行緒安全）
void ModeDryFire::webStart() {
  UiCmdMsg m; m.cmd = WebCmd::START; m.beatMs = 0;
  IPC::sendUiCmd(m);
}

/// @brief Web 請求停止（透過 IPC queue，執行緒安全）
void ModeDryFire::webStop() {
  UiCmdMsg m; m.cmd = WebCmd::STOP; m.beatMs = 0;
  IPC::sendUiCmd(m);
}

/**
 * @brief 在 loop() Task 中套用新的 beat interval（由 GameFSM::processWebCmd 呼叫）
 *
 * 此函式在 loop() Task（Core 1）執行，可安全操作 UI。
 * 更新 _beatMs、重繪畫面並持久化到 NVS。
 *
 * @param ms  新的 beat interval（ms）
 */
void ModeDryFire::applySetBeat(uint32_t ms) {
  if (!s_dfInstance) return;
  ms = constrain(ms, Timing::DRY_BEAT_MIN_MS, Timing::DRY_BEAT_MAX_MS);
  s_dfInstance->_beatMs = ms;
  s_dfInstance->drawScreen();
  s_dfInstance->saveBeatMs();
}

/// @brief 查詢節拍 session 是否正在執行
/// @return true 正在執行，false 未執行或無實例
bool ModeDryFire::isRunning() {
  return s_dfInstance ? s_dfInstance->_running : false;
}

/// @brief 從外部（GameFSM/Web）請求開始 session（已在 loop Task 執行）
void ModeDryFire::doStart() {
  if (!s_dfInstance || s_dfInstance->_running) return;
  s_dfInstance->startSession();
}

/// @brief 從外部（GameFSM/Web）請求停止 session（已在 loop Task 執行）
void ModeDryFire::doStop() {
  if (!s_dfInstance || !s_dfInstance->_running) return;
  s_dfInstance->stopSession();
  s_dfInstance->drawScreen();
}


/**
 * @brief 按鍵事件處理
 *
 * - BtnA (0)：停止（running）/ 返回 Home（standby）
 * - BtnB (1)：調慢 +200ms（上限 DRY_BEAT_MAX_MS）
 * - BtnC (2)：START（standby）/ 調快 -200ms（running）
 *
 * @param btn  按鍵索引（0=A / 1=B / 2=C）
 */
void ModeDryFire::onButton(uint8_t btn) {
  switch (btn) {
    case 0:  // BtnA — Stop / Home
      if (_running) { stopSession(); drawScreen(); }
      else GameFSM::requestSwitch(AppMode::HOME);
      break;
    case 1:  // BtnB — 調慢
      if (_beatMs + 200 <= Timing::DRY_BEAT_MAX_MS) {
        _beatMs += 200;
        drawScreen();
        saveBeatMs();
      }
      break;
    case 2:  // BtnC — Start / 調快
      if (!_running) {
        startSession();
      } else {
        if (_beatMs >= Timing::DRY_BEAT_MIN_MS + 200) {
          _beatMs -= 200;
          drawScreen();
          saveBeatMs();
        }
      }
      break;
    default: break;
  }
}

/// @brief 觸控事件（DRY FIRE 模式無遊戲區觸控）
void ModeDryFire::onTouch(int16_t /*x*/, int16_t /*y*/) {
  // 無遊戲區觸控（按鍵由 btn bar 處理）
}

/// @brief 完整重繪 DRY FIRE 畫面（header / beat info / btn bar）
void ModeDryFire::drawScreen() {
  UIScreen::clearGameArea();
  UIScreen::drawModeHeader("DRY FIRE", _running ? "RUNNING" : "READY", 0xA000);
  unsigned long elapsed = _running ? (millis() - _sessionStart) : 0;
  UIScreen::drawDryFireScreen(_beatMs, _beatCount, elapsed, _running);
  UIScreen::drawBtnBar(_running ? "STOP" : "HOME",
                       "SLOWER",
                       _running ? "FASTER" : "START");
}

/**
 * @brief 開始節拍 session
 *
 * 立即播放第一聲 tick，記錄 session 起始時間，並更新畫面。
 */
void ModeDryFire::startSession() {
  _running      = true;
  _beatCount    = 0;
  _sessionStart = millis();
  _lastBeatAt   = millis();

  // 第一聲立即響
  playAudio(SND::TT2_DRY_TICK);
  _beatCount++;

  drawScreen();
  Serial.printf("[ModeDryFire] Session started, beat=%ums\n", _beatMs);
}

/// @brief 停止節拍 session，停止音效播放
void ModeDryFire::stopSession() {
  _running = false;
  stopAudio();
  Serial.printf("[ModeDryFire] Session stopped: %u beats in %lums\n",
                _beatCount, millis() - _sessionStart);
}

/**
 * @brief 節拍驅動函式（每幀由 update() 呼叫）
 *
 * 檢查距上次 tick 是否已超過 _beatMs，
 * 若是則播放 tick 音效並累加 _beatCount。
 * 每 5 拍更新一次畫面，避免頻繁重繪造成閃爍。
 */
void ModeDryFire::tick() {
  if (!_running) return;
  unsigned long now = millis();
  if (now - _lastBeatAt >= _beatMs) {
    _lastBeatAt = now;
    playAudio(SND::TT2_DRY_TICK);
    _beatCount++;

    // 每 5 拍更新一次畫面（避免頻繁重繪）
    if (_beatCount % 5 == 0) drawScreen();
  }
}
