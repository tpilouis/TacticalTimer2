/**
 * @file    web_server.h
 * @brief   TACTICAL TIMER 2 — Web 伺服器宣告
 *
 * 提供 AsyncWebServer 初始化和所有 SSE 推播函式的宣告。
 *
 * HTTP 路由（全部在 WebServer::init() 中註冊）：
 *  - GET  /                → Web Dashboard HTML
 *  - GET  /state           → 目前遊戲狀態 JSON
 *  - POST /cmd             → 遊戲控制（start / stop / rand / par / reboot）
 *  - POST /drill           → DRILL 課題設定
 *  - POST /dryfire         → DRY FIRE beat interval
 *  - POST /spy             → SPY listen / stop / clear
 *  - POST /setting         → Threshold / HitSource 更新
 *  - GET  /preset          → 取得所有 Preset 列表
 *  - POST /preset          → 切換 active Preset
 *  - GET  /history/detail  → 單筆 session 詳情（需在 /history 前）
 *  - GET  /history         → Session 列表（最新 50 筆）
 *  - POST /history/delete  → 刪除指定 session
 *  - SSE  /events          → SSE 事件流
 *
 * SSE 事件清單：
 *  - `hit`        → 命中事件（hitIdx / stationId / elapsed / split）
 *  - `gs`         → GameState 整數
 *  - `newgame`    → 新局開始（清空 shot log）
 *  - `best`       → 最佳紀錄更新（t / a）
 *  - `mode`       → AppMode 整數
 *  - `drill`      → DRILL 課題定義（shots / parMs / passPct）
 *  - `drillresult`→ DRILL 結果（score / passed）
 *  - `drybeat`    → DRY FIRE beat interval（ms）
 *  - `settings`   → Settings 變更（preset / thresh / src）
 *  - `partime`    → Par Time 變更（parMs / rand）
 *
 * @version 3.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include "ipc.h"
#include "timer_core.h"
#include "preset_mgr.h"
#include "session_mgr.h"
#include "hal_storage.h"
#include "game_fsm.h"
#include "mode_drill.h"
#include "mode_dryfire.h"


namespace WebServer {

  /**
   * @brief 初始化 AsyncWebServer，註冊所有 HTTP 路由和 SSE endpoint
   *
   * 必須在 WiFi 連線後呼叫。
   */
  void init();

  /** @brief 推播 GameState 整數（SSE event: "gs"） */
  void sendGameState();

  /** @brief 推播新局開始信號（SSE event: "newgame"，清空 Web shot log） */
  void sendNewGame();

  /**
   * @brief 推播最佳紀錄更新（SSE event: "best"）
   * @param totalSec  最佳總時間（秒）
   * @param avgSec    最佳平均 split（秒）
   */
  void sendBest(float totalSec, float avgSec);

  /**
   * @brief 推播命中事件（SSE event: "hit"）
   * @param sm  SseMsg（hitIdx / stationId / elapsed / split）
   */
  void sendHit(const SseMsg& sm);

  /**
   * @brief 推播模式切換（SSE event: "mode"）
   * @param mode  目前 AppMode
   */
  void sendMode(AppMode mode);

  /**
   * @brief 推播 DRILL 課題定義（SSE event: "drill"）
   * @param shots    目標發數
   * @param parMs    Par Time（ms）
   * @param passPct  及格百分比
   */
  void sendDrillDef(uint8_t shots, uint32_t parMs, uint8_t passPct);

  /**
   * @brief 推播 DRILL 結果（SSE event: "drillresult"）
   * @param score    達成率（0.0–100.0）
   * @param passed   是否通過
   * @param shots    目標發數
   * @param parMs    Par Time（ms）
   * @param passPct  及格百分比
   */
  void sendDrillResult(float score, bool passed,
                       uint8_t shots, uint32_t parMs, uint8_t passPct);

  /**
   * @brief 推播 DRY FIRE beat interval（SSE event: "drybeat"）
   * @param beatMs  節拍間隔（ms）
   */
  void sendDryBeatMs(uint32_t beatMs);

  /**
   * @brief 推播 Settings 變更（SSE event: "settings"）
   *
   * Core2 或 Web 調整 Preset / Threshold / Source 後呼叫，確保雙向同步。
   *
   * @param activePreset  active preset 索引（0–4）
   * @param micThresh     MIC RMS 門檻
   * @param hitSource     HitSource 枚舉值（uint8_t）
   */
  void sendSettings(uint8_t activePreset, int16_t micThresh, uint8_t hitSource);

  /**
   * @brief 推播 Par Time / Random Delay 變更（SSE event: "partime"）
   *
   * Core2 PAR SET 或 Web 調整後呼叫，確保雙向同步。
   *
   * @param parMs        Par Time（ms），0 = 停用
   * @param randomDelay  Random Delay 是否啟用
   */
  void sendParTime(uint32_t parMs, bool randomDelay);

} // namespace WebServer

/**
 * @brief C 介面的 sendHit 包裝（供 NetworkMgr 呼叫，避免循環 include）
 * @param sm  SseMsg 命中資料
 */
void webServerSendHit(const SseMsg& sm);
