/**
 * @file    mode_spy.cpp
 * @brief   TACTICAL TIMER 2 — Spy Mode 實作
 *
 * Spy Mode 為被動監聽模式：MicEngine 不受 GameState 限制，
 * 任何時候都可偵測音爆，記錄 split time 並顯示統計。
 * 本模式不儲存 session（設計決策）。
 *
 * @version 2.0.0
 */

#include "mode_spy.h"
#include "ui_screen.h"
#include "game_fsm.h"
#include "web_server.h"
#include <Arduino.h>
#include <cstring>

/// @cond INTERNAL
/// 模組單例指標（供靜態 callback 存取實例成員）
static ModeSpy* s_spyInstance = nullptr;
/// @endcond

namespace UIScreen {
  void drawModeHeader(const char*, const char*, uint16_t);
  void clearGameArea();
  void drawBtnBar(const char*, const char*, const char*);
  void drawSpyScreen(uint8_t shotCount, unsigned long totalMs,
                     const void* hits, bool listening);
}


/**
 * @brief 進入 SPY 模式，重置所有紀錄並啟動 MicEngine Spy Mode
 */
void ModeSpy::onEnter() {
  s_spyInstance = this;
  _listening     = false;
  _shotCount     = 0;
  _listenStartAt = 0;
  _firstShotAt   = 0;
  _lastShotAt    = 0;
  memset(_hits, 0, sizeof(_hits));
  UIScreen::resetShotPage();

  MicEngine::setSpyMode(true);

  drawScreen();
  Serial.println("[ModeSpy] onEnter, SpyMode=ON");
}

/**
 * @brief 每幀更新：從 hitQueue 消費命中事件（僅 listening 時）
 *
 * 不走 TimerCore（TimerCore 要求 GOING 狀態），
 * 直接從 IPC::hitQueue 取出 HitMsg，計算 split 後更新畫面和 SSE。
 *
 * Split 計算規則：
 *  - 第一槍：split = now - _listenStartAt（等待時間）
 *  - 其後：split = now - _lastShotAt（分段時間）
 */
void ModeSpy::update() {
  if (!_listening) return;

  // Spy Mode 命中由 MicEngine 送入 hitQueue
  // 但不走 TimerCore（TimerCore 要求 GOING）
  // 改直接從 hitQueue 取出
  HitMsg hm;
  while (IPC::receiveHit(hm)) {
    if (_shotCount >= Limits::HIT_MAX) break;

    unsigned long now   = hm.hit_time_ms;
    unsigned long split = (_shotCount == 0)
                          ? (now - _listenStartAt)   // 第一槍：從 LISTEN 到第一槍
                          : (now - _lastShotAt);     // 其後：與上一槍的分段

    if (_shotCount == 0) _firstShotAt = now;
    _hits[_shotCount].timestamp = now;
    _hits[_shotCount].split     = split;
    _lastShotAt = now;
    _shotCount++;

    Serial.printf("[ModeSpy] Shot #%u split=%lu\n", _shotCount, split);

    // SSE 推播到 Web Dashboard
    // elapsed = 從第一槍到當槍（與 Core2 螢幕一致）
    // 第一槍：elapsed=0, split=等待時間（與 Core2 一致，不加+號）
    unsigned long elapsedFromFirst = (_firstShotAt > 0 && _shotCount > 1)
                                     ? (now - _firstShotAt) : 0;
    SseMsg sm;
    sm.hitIdx    = _shotCount;
    sm.stationId = hm.station_id;
    sm.elapsed   = elapsedFromFirst;
    sm.split     = split;   // 第一槍=等待時間，其後=分段時間
    IPC::sendSse(sm);

    // 每次命中更新畫面
    unsigned long totalMs = (_firstShotAt > 0 && _shotCount > 0)
                            ? _lastShotAt - _firstShotAt : 0;
    UIScreen::drawSpyScreen(_shotCount, totalMs, _hits, _listening);
  }
}

/// @brief 離開 SPY 模式，停用 MicEngine Spy Mode
void ModeSpy::onExit() {
  MicEngine::setSpyMode(false);
  s_spyInstance = nullptr;
  Serial.println("[ModeSpy] onExit, SpyMode=OFF");
}

/**
 * @brief 按鍵事件處理
 *
 * - BtnA (0)：停止監聽（listening 時）/ 返回 Home（standby 時）
 * - BtnB (1)：清除所有命中紀錄
 * - BtnC (2)：開始監聽（standby 時）/ 暫停監聽（listening 時）
 *
 * @param btn  按鍵索引（0=A / 1=B / 2=C）
 */
void ModeSpy::onButton(uint8_t btn) {
  switch (btn) {
    case 0:  // BtnA — 停止監聽 / 上一頁 / Home
      if (_listening) {
        stopListening();
        // 停止後保留當前頁面，讓用戶瀏覽
        drawScreen();
      } else if (UIScreen::shotPageHasPrev()) {
        UIScreen::shotPagePrev();
        drawScreen();
      } else {
        GameFSM::requestSwitch(AppMode::HOME);
      }
      break;
    case 1:  // BtnB — 清除紀錄
      _shotCount    = 0;
      _listenStartAt = 0;
      _firstShotAt  = 0;
      _lastShotAt   = 0;
      memset(_hits, 0, sizeof(_hits));
      UIScreen::resetShotPage();
      drawScreen();
      break;
    case 2:  // BtnC — 下一頁 / 開始監聽 / 暫停
      if (!_listening && UIScreen::shotPageHasNext()) {
        UIScreen::shotPageNext();
        drawScreen();
      } else if (!_listening) {
        // 無下頁且未監聽：開始新一輪監聽
        UIScreen::resetShotPage();
        startListening();
        drawScreen();
      } else {
        // 監聽中：暫停
        stopListening();
        drawScreen();
      }
      break;
    default: break;
  }
}

/// @brief 觸控事件（SPY 模式無遊戲區觸控，僅由 btn bar 處理）
void ModeSpy::onTouch(int16_t /*x*/, int16_t /*y*/) {
  // 無遊戲區觸控（按鍵由 btn bar 處理）
}

/**
 * @brief 開始監聽：設定旗標、記錄起始時間，通知 Web 清空 shot log
 */
void ModeSpy::startListening() {
  _listening     = true;
  _listenStartAt = millis();
  WebServer::sendNewGame();   // Web Dashboard 清空 shot log
  Serial.printf("[ModeSpy] Listening started at %lu\n", _listenStartAt);
}

/// @brief 停止監聽：清除旗標
void ModeSpy::stopListening() {
  _listening = false;
  Serial.println("[ModeSpy] Listening stopped");
}

/**
 * @brief 完整重繪 SPY 模式畫面
 *
 * 依據 _listening 狀態顯示 LISTENING / STANDBY badge，
 * 以及目前 shot count 和 split times。
 */
void ModeSpy::drawScreen() {
  UIScreen::clearGameArea();
  UIScreen::drawModeHeader("SPY MODE",
                           _listening ? "LISTENING" : "STANDBY",
                           _listening ? 0x07E0 : 0x5AB0);
  unsigned long totalMs = (_firstShotAt > 0 && _shotCount > 0)
                          ? _lastShotAt - _firstShotAt : 0;
  UIScreen::drawSpyScreen(_shotCount, totalMs, _hits, _listening);

  if (_listening) {
    UIScreen::drawBtnBar("STOP", "CLEAR", "PAUSE");
  } else if (_shotCount > 0) {
    // 停止後有成績：顯示翻頁按鈕
    const char* lblA = UIScreen::shotPageHasPrev() ? "< PREV" : "HOME";
    const char* lblC = UIScreen::shotPageHasNext() ? "NEXT >" : "LISTEN";
    UIScreen::drawBtnBar(lblA, "CLEAR", lblC);
  } else {
    UIScreen::drawBtnBar("HOME", "CLEAR", "LISTEN");
  }
}

/**
 * @brief 預留的靜態命中 callback（目前未使用）
 *
 * SPY 模式的命中在 update() 中直接從 hitQueue 消費，
 * 此函式保留供未來擴充使用。
 */
void ModeSpy::onSpyHit(uint32_t, uint8_t, unsigned long, unsigned long) {
  // Spy 模式的命中在 update() 中直接從 hitQueue 消費
  // 此 callback 預留給未來擴充
}
