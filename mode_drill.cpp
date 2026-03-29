/**
 * @file    mode_drill.cpp
 * @brief   TACTICAL TIMER 2 — Drills 模式實作
 *
 * 課題訓練模式，支援：
 *  - 自訂發數（1–HIT_MAX）、Par Time、Pass % 門檻
 *  - 四個 Phase：SETUP → RUNNING → RESULT → REVIEW
 *  - 每發即時判斷 Pass/Fail（split ≤ par/shots）
 *  - 完成後儲存 session（含 score / passed / presetName）
 *
 * @version 2.0.0
 */

#include "mode_drill.h"
#include "game_fsm.h"
#include "web_server.h"
#include "hal_storage.h"
#include "preset_mgr.h"
#include <Arduino.h>
#include <cstring>

/// @cond INTERNAL
/// 模組單例指標（靜態 callback 需要存取 instance）
static ModeDrill* s_instance = nullptr;
/// @endcond

namespace UIScreen {
  void drawModeHeader(const char*, const char*, uint16_t);
  void drawShotRow(uint8_t, uint8_t, unsigned long, unsigned long, bool pass = true);
  void clearGameArea();
  void drawBtnBar(const char*, const char*, const char*);
  void drawDrillSetup(const DrillDef&);
  void drawDrillResult(const DrillResult&);
}


// ============================================================
// [DRILL-impl] ModeBase 介面
// ============================================================

/**
 * @brief 進入 DRILL 模式，從 AppSettings 載入課題設定，顯示 SETUP 畫面
 */
void ModeDrill::onEnter() {
  s_instance = this;
  _phase     = DrillPhase::SETUP;
  _passCount = 0;

  // 從 AppSettings 載入上次儲存的課題設定
  memset(&_def, 0, sizeof(_def));
  AppSettings cfg = PresetMgr::getEffectiveSettings();
  _def.shotCount   = (cfg.drillShots > 0 && cfg.drillShots <= Limits::HIT_MAX)
                     ? cfg.drillShots : 6;
  _def.parTimeMs   = cfg.drillParMs;
  _def.passPct     = (cfg.drillPassPct <= 100) ? cfg.drillPassPct : 80;
  _def.randomDelay = cfg.randomDelay;
  _def.level       = DrillLevel::INTERMEDIATE;
  // 自動產生課題名稱
  if (_def.parTimeMs > 0) {
    snprintf(_def.name, sizeof(_def.name), "%d Shots %.1fs",
             _def.shotCount, _def.parTimeMs / 1000.0f);
  } else {
    snprintf(_def.name, sizeof(_def.name), "%d Shots", _def.shotCount);
  }

  drawSetupScreen();
  Serial.printf("[ModeDrill] onEnter: shots=%d par=%dms pass=%d%%\n",
                _def.shotCount, _def.parTimeMs, _def.passPct);
}

/**
 * @brief 每幀更新（僅 RUNNING phase 有效，驅動 TimerCore process 函式）
 */
void ModeDrill::update() {
  if (_phase != DrillPhase::RUNNING) return;

  TimerCore::processHits(onHit);
  TimerCore::processAudioDone(onReadyDone, onBeepDone, onClearDone);
  TimerCore::processDelay(onDelayExpired);
}

/// @brief 離開 DRILL 模式，清除單例指標，若計時中則強制停止
void ModeDrill::onExit() {
  s_instance = nullptr;
  GameState gs = TimerCore::getState();
  if (gs == GameState::GOING || gs == GameState::AREYOUREADY ||
      gs == GameState::BEEPING || gs == GameState::WAITING) {
    AudioCmd cmd; cmd.op = AudioCmd::Op::STOP;
    IPC::sendAudioCmd(cmd);
    TimerCore::forceStop();
  }
  Serial.println("[ModeDrill] onExit");
}

/**
 * @brief 按鍵事件處理（依 Phase 不同行為不同）
 *
 * SETUP：BtnA=HOME / BtnB=SETTINGS / BtnC=START
 * RUNNING：BtnA=STOP / BtnC=RESTART
 * RESULT：BtnA=HOME / BtnB=SHOTS（REVIEW）/ BtnC=RETRY
 * REVIEW：BtnB=返回 RESULT
 *
 * @param btn  按鍵索引（0=A / 1=B / 2=C）
 */
void ModeDrill::onButton(uint8_t btn) {
  GameState gs = TimerCore::getState();

  if (_phase == DrillPhase::SETUP) {
    switch (btn) {
      case 0:  // BtnA — 返回 HOME
        GameFSM::requestSwitch(AppMode::HOME);
        break;
      case 1:  // BtnB — 調整設定
        GameFSM::requestSwitch(AppMode::SETTINGS);
        break;
      case 2:  // BtnC — 開始
        startRun();
        break;
      default: break;
    }
    return;
  }

  if (_phase == DrillPhase::RUNNING) {
    switch (btn) {
      case 0:  // BtnA — 強制停止
        if (gs == GameState::GOING) {
          stopAudio();
          TimerCore::forceStop();
          WebServer::sendGameState();
        }
        break;
      case 2:  // BtnC — 重新開始
        startRun();
        break;
      default: break;
    }
    return;
  }

  if (_phase == DrillPhase::RESULT) {
    switch (btn) {
      case 0: GameFSM::requestSwitch(AppMode::HOME); break;  // HOME
      case 1: _phase = DrillPhase::REVIEW; drawReviewScreen(); break;  // SHOTS
      case 2: _phase = DrillPhase::SETUP; drawSetupScreen(); break;  // RETRY
      default: break;
    }
    return;
  }

  if (_phase == DrillPhase::REVIEW) {
    switch (btn) {
      case 1: _phase = DrillPhase::RESULT; drawResultScreen(); break;  // 返回結果頁
      default: break;
    }
    return;
  }
}

/**
 * @brief 觸控事件處理（僅 SETUP phase）
 *
 * Row 0（Shots）：[-]/[+] 調整發數（1–HIT_MAX）
 * Row 1（Par Time）：[-]/[+] 調整時間（0–PAR_TIME_MAX_MS，步進 PAR_TIME_STEP_MS）
 * Row 3（Random Delay）：整行觸控切換 ON/OFF
 *
 * 每次調整後立即呼叫 saveDef() 持久化。
 *
 * @param x  觸控 X 座標
 * @param y  觸控 Y 座標
 */
void ModeDrill::onTouch(int16_t x, int16_t y) {
  // SETUP 畫面觸控：對應 drawDrillSetup 的新佈局
  // ROW_H=40, Y0 = GAME_AREA_Y + MODE_HDR_H + 4 = 42
  // [-] x=84~124, [+] x=276~316, 各 Row Y 範圍固定 40px
  if (_phase != DrillPhase::SETUP) return;

  constexpr int16_t Y0      = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 4; // 42
  constexpr int16_t ROW_H   = 40;
  constexpr int16_t BTN_L_X = 84;   // [-] 起點
  constexpr int16_t BTN_W   = 40;
  constexpr int16_t BTN_R_X = 276;  // [+] 起點

  // Row 0：Shots
  if (y >= Y0 && y <= Y0 + ROW_H) {
    if (x >= BTN_L_X && x <= BTN_L_X + BTN_W) {
      if (_def.shotCount > 1) { _def.shotCount--; drawSetupScreen(); saveDef(); }
    } else if (x >= BTN_R_X && x <= BTN_R_X + BTN_W) {
      if (_def.shotCount < Limits::HIT_MAX) { _def.shotCount++; drawSetupScreen(); saveDef(); }
    }
    return;
  }

  // Row 1：Par Time
  if (y >= Y0 + ROW_H && y <= Y0 + 2 * ROW_H) {
    if (x >= BTN_L_X && x <= BTN_L_X + BTN_W) {
      if (_def.parTimeMs >= Timing::PAR_TIME_STEP_MS)
        _def.parTimeMs -= Timing::PAR_TIME_STEP_MS;
      else
        _def.parTimeMs = 0;
      drawSetupScreen(); saveDef();
    } else if (x >= BTN_R_X && x <= BTN_R_X + BTN_W) {
      if (_def.parTimeMs < Timing::PAR_TIME_MAX_MS)
        _def.parTimeMs += Timing::PAR_TIME_STEP_MS;
      drawSetupScreen(); saveDef();
    }
    return;
  }

  // Row 3：Random toggle（整行觸控）
  if (y >= Y0 + 3 * ROW_H && y <= Y0 + 4 * ROW_H) {
    _def.randomDelay = !_def.randomDelay;
    drawSetupScreen(); saveDef();
  }
}


// ============================================================
// [DRILL-impl] 私有方法
// ============================================================

/// @brief 繪製 SETUP 畫面（發數、Par Time、Pass % 設定）
void ModeDrill::drawSetupScreen() {
  UIScreen::clearGameArea();
  UIScreen::drawModeHeader("DRILLS", "SETUP", 0xFFC0);
  UIScreen::drawDrillSetup(_def);
  UIScreen::drawBtnBar("HOME", "SETTINGS", "START");
}

/**
 * @brief 儲存目前 _def 到 AppSettings NVS，並推播 SSE 讓 Web 同步
 *
 * 寫入 drillShots / drillParMs / drillPassPct / randomDelay。
 */
void ModeDrill::saveDef() {
  AppSettings s = PresetMgr::getEffectiveSettings();
  s.drillShots   = _def.shotCount;
  s.drillParMs   = _def.parTimeMs;
  s.drillPassPct = _def.passPct;
  s.randomDelay  = _def.randomDelay;
  HalStorage::saveSettings(s);
  PresetMgr::applySettings(s);
  // 推播 SSE 讓 Web 同步最新 _def
  WebServer::sendDrillDef(_def.shotCount, _def.parTimeMs, _def.passPct);
  Serial.printf("[ModeDrill] saveDef: shots=%d par=%dms pass=%d%%\n",
                _def.shotCount, _def.parTimeMs, _def.passPct);
}

/**
 * @brief 取得目前 _def 的指標（供 web_server 讀取）
 * @return 目前 DrillDef 指標，無實例時回傳 nullptr
 */
const DrillDef* ModeDrill::getDef() {
  return s_instance ? &s_instance->_def : nullptr;
}

/**
 * @brief Web 請求開始（從 AppSettings 重新載入設定後啟動）
 *
 * 無論目前在哪個 phase 都可呼叫，確保 Web 設定與 Core2 一致。
 */
void ModeDrill::webStart() {
  if (!s_instance) return;
  // 重新從 AppSettings 載入最新設定到 _def
  AppSettings cfg = PresetMgr::getEffectiveSettings();
  s_instance->_def.shotCount   = (cfg.drillShots > 0 && cfg.drillShots <= Limits::HIT_MAX)
                                  ? cfg.drillShots : 6;
  s_instance->_def.parTimeMs   = cfg.drillParMs;
  s_instance->_def.passPct     = (cfg.drillPassPct <= 100) ? cfg.drillPassPct : 80;
  s_instance->_def.randomDelay = cfg.randomDelay;
  s_instance->startRun();
}

/**
 * @brief Web 調整設定後同步到 Core2 的 _def
 *
 * 若目前在 SETUP phase，同時重繪 Core2 螢幕。
 *
 * @param shots   目標發數
 * @param parMs   Par Time（ms）
 * @param passPct 及格百分比
 */
void ModeDrill::updateDef(uint8_t shots, uint32_t parMs, uint8_t passPct) {
  if (!s_instance) return;
  s_instance->_def.shotCount = shots;
  s_instance->_def.parTimeMs = parMs;
  s_instance->_def.passPct   = passPct;
  // 若在 SETUP phase，重繪畫面讓 Core2 螢幕同步
  if (s_instance->_phase == DrillPhase::SETUP) {
    s_instance->drawSetupScreen();
  }
}

/// @brief 繪製 RESULT 畫面（PASSED/FAILED + 成績統計）
void ModeDrill::drawResultScreen() {
  UIScreen::clearGameArea();
  UIScreen::drawDrillResult(_result);
  UIScreen::drawBtnBar("HOME", "SHOTS", "RETRY");
}

/**
 * @brief 繪製 REVIEW 畫面（重新顯示所有 shot rows，含 Pass/Fail 標記）
 *
 * 重新計算每發的 elapsed/split 並呼叫 judgeShot() 判斷顏色。
 */
void ModeDrill::drawReviewScreen() {
  // 重繪射擊畫面（Shot Row 已在 RUNNING 時畫好，只需重繪 header 和 btn bar）
  UIScreen::clearGameArea();
  UIScreen::drawModeHeader("DRILLS", _result.passed ? "PASSED" : "FAILED",
                           _result.passed ? 0x07E0 : 0xF800);

  // 重繪各發 Shot Row
  const GameRecord& rec = _result.record;
  for (uint8_t i = 0; i < rec.hit_count && i < _def.shotCount; i++) {
    uint8_t  hitIdx   = i + 1;
    unsigned long elapsed = 0, split = 0;
    // 計算 elapsed 和 split
    elapsed = rec.hits[i].hit_time_ms - rec.start_time_ms;
    split   = (i == 0) ? elapsed
                       : rec.hits[i].hit_time_ms - rec.hits[i-1].hit_time_ms;
    bool pass = judgeShot(hitIdx, split);
    UIScreen::drawShotRow(hitIdx, rec.hits[i].station_id, elapsed, split, pass);
  }

  UIScreen::drawBtnBar(nullptr, "RESULT", nullptr);
}

/**
 * @brief 開始一輪課題訓練
 *
 * 設定 RUNNING phase，注入 par time / random delay 到 TimerCore，
 * 呼叫 startNew() 並播放 READY 音效。
 */
void ModeDrill::startRun() {
  _phase     = DrillPhase::RUNNING;
  _passCount = 0;

  // 將課題設定注入 TimerCore
  TimerCore::setParTime(_def.parTimeMs);
  TimerCore::setRandomDelay(_def.randomDelay);

  uint8_t gi = TimerCore::startNew();
  playAudio(SND::TT2_READY);
  WebServer::sendNewGame();
  WebServer::sendGameState();

  UIScreen::clearGameArea();
  UIScreen::drawModeHeader("DRILLS", "READY", 0xFFC0);
  UIScreen::drawBtnBar("STOP", "---", "RESTART");

  Serial.printf("[ModeDrill] startRun: shots=%u par=%ums gi=%u\n",
                _def.shotCount, _def.parTimeMs, gi);
}

/**
 * @brief 判斷單發是否 Pass
 *
 * 規則：
 *  - 第一發（反應時間）永遠 Pass
 *  - parTimeMs == 0（無 par）永遠 Pass
 *  - split ≤ (parTimeMs / shotCount) 為 Pass
 *
 * @param hitIdx  本局第幾槍（1-based）
 * @param split   本發分段時間（ms）
 * @return true = Pass，false = Fail
 */
bool ModeDrill::judgeShot(uint8_t hitIdx, unsigned long split) const {
  if (hitIdx == 1) return true;          // 第一發 = 反應時間，不納入評分
  if (_def.parTimeMs == 0) return true;  // 無 par → 永遠 Pass
  // 分段時間 ≤ (par / shots) 視為 Pass
  uint32_t threshold = _def.parTimeMs / _def.shotCount;
  return split <= threshold;
}


// ============================================================
// [DRILL-impl] 靜態 callback
// ============================================================

/**
 * @brief 命中事件 callback（由 TimerCore::processHits 呼叫）
 *
 * 呼叫 judgeShot() 判斷 Pass/Fail，累加 _passCount，
 * 更新 shot row，推播 SSE。
 * 達到目標發數時立即 forceStop() 並播放 Clear 音效。
 *
 * @param hitIdx    本局第幾槍（1-based）
 * @param stationId 命中來源
 * @param elapsed   累積時間（ms）
 * @param split     分段時間（ms）
 */
void ModeDrill::onHit(uint8_t hitIdx, uint8_t stationId,
                      unsigned long elapsed, unsigned long split) {
  if (!s_instance) return;
  bool pass = s_instance->judgeShot(hitIdx, split);
  if (pass) s_instance->_passCount++;

  UIScreen::drawShotRow(hitIdx, stationId, elapsed, split, pass);

  SseMsg sm = { hitIdx, stationId, elapsed, split };
  IPC::sendSse(sm);

  // 達到目標發數 → 停止計時，播放 Clear
  if (hitIdx >= s_instance->_def.shotCount) {
    TimerCore::forceStop();  // 立即停止，MicEngine 不再接受命中
    AudioCmd cmd; cmd.op = AudioCmd::Op::PLAY;
    strlcpy(cmd.path, SND::TT2_CLEAR, sizeof(cmd.path));
    IPC::sendAudioCmd(cmd);
    Serial.printf("[ModeDrill] Target shots reached (%u), forceStop\n",
                  s_instance->_def.shotCount);
  }

  Serial.printf("[ModeDrill] Hit #%u %s split=%lu\n",
                hitIdx, pass ? "PASS" : "FAIL", split);
}

/// @brief AREYOUREADY 音效播完 → 標頭顯示 WAITING
void ModeDrill::onReadyDone() {
  UIScreen::drawModeHeader("DRILLS", "WAITING", 0xFFC0);
}

/// @brief START beep 播完 → 標頭顯示 GOING，通知 Web GameState
void ModeDrill::onBeepDone() {
  UIScreen::drawModeHeader("DRILLS", "GOING", 0x07E0);
  WebServer::sendGameState();
}

/**
 * @brief CLEAR 音效播完 → 計算成績、儲存 session、顯示 RESULT
 *
 * 計算 score（passCount / shots × 100）和 passed（score >= passPct），
 * 填入 _result 後儲存 session（含 drillScore / drillPassed / presetName），
 * 切換到 RESULT phase 並推播 SSE。
 */
void ModeDrill::onClearDone() {
  if (!s_instance) return;

  const GameRecord* rec = TimerCore::getRecord(0);
  uint8_t shots         = rec ? rec->hit_count : 0;

  // 填寫 DrillResult
  s_instance->_result.def       = s_instance->_def;
  if (rec) s_instance->_result.record = *rec;
  s_instance->_result.passCount = s_instance->_passCount;
  s_instance->_result.score     = shots > 0
    ? (float)s_instance->_passCount / shots * 100.0f : 0.0f;
  s_instance->_result.passed    = s_instance->_result.score
                                 >= s_instance->_def.passPct;

  // 儲存 session（傳入課題定義，供 History 頁面顯示 Pass/Fail）
  if (rec) {
    unsigned long totalMs2 = TimerCore::calcTotalMs(*rec);
    if (totalMs2 > 0 && rec->hit_count > 0) {
      float tSec = totalMs2 / 1000.0f;
      TimerCore::updateBest(tSec, tSec / rec->hit_count);
      WebServer::sendBest(TimerCore::getBestTotal(), TimerCore::getBestAvg());
    }
    // 存檔附上 DrillDef + SessionExtra
    SessionExtra extra;
    extra.drillScore  = s_instance->_result.score;
    extra.drillPassed = s_instance->_result.passed;
    const Preset& pr  = PresetMgr::getActive();
    extra.presetName  = pr.name[0] ? pr.name : nullptr;
    SessionMgr::saveSession(*rec, AppMode::DRILL, &s_instance->_def, &extra);
  }

  s_instance->_phase = DrillPhase::RESULT;
  s_instance->drawResultScreen();
  // 推播 Drill 結果到 Web
  WebServer::sendDrillResult(
    s_instance->_result.score,
    s_instance->_result.passed,
    s_instance->_def.shotCount,
    s_instance->_def.parTimeMs,
    s_instance->_def.passPct);
  WebServer::sendGameState();

  Serial.printf("[ModeDrill] Done: score=%.1f%% passed=%d\n",
                s_instance->_result.score,
                (int)s_instance->_result.passed);
}

/**
 * @brief 延遲計時到期 callback（僅處理 BEEPING → 播放 beep）
 * @param pending  即將轉入的 GameState
 */
void ModeDrill::onDelayExpired(GameState pending) {
  if (pending == GameState::BEEPING) {
    AudioCmd cmd; cmd.op = AudioCmd::Op::PLAY;
    strlcpy(cmd.path, SND::TT2_BEEP, sizeof(cmd.path));
    IPC::sendAudioCmd(cmd);
    UIScreen::drawModeHeader("DRILLS", "BEEP!", 0x07E0);
  }
}
