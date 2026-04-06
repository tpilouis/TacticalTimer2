/**
 * @file    session_mgr.h
 * @brief   TACTICAL TIMER 2 — Session 管理服務宣告
 *
 * 職責：
 *  - 每局結束時將 GameRecord 持久化至 SD 卡（透過 HalStorage）
 *  - 維護 RAM 中的最佳紀錄（bestTotal / bestAvg）
 *  - 提供 History 頁面所需的 session 列表與詳情讀取
 *  - Observer 回呼通知 Web 層推播最佳紀錄更新
 *
 * 設計模式：Repository（session CRUD）+ Observer（最佳更新通知）
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include "hal_storage.h"
#include "timer_core.h"


namespace SessionMgr {

  /** @brief 初始化（initSD + 快取 session 數量）*/
  bool init();

  /**
   * @brief 儲存最近一局至 SD（每局結束時呼叫）
   *
   * @param mode         本局模式
   * @param onBestUpdate 最佳更新回呼 (bestTotal, bestAvg)，nullptr = 略過
   * @return true = 成功
   */
  bool saveLastSession(AppMode mode,
                       void (*onBestUpdate)(float, float) = nullptr,
                       const SessionExtra* extra = nullptr);

  bool saveSession(const GameRecord& record, AppMode mode,
                   const DrillDef* drillDef = nullptr,
                   const SessionExtra* extra = nullptr);

  /** @brief Session 列表（History Mode 使用）*/
  bool listSessions(char paths[][Limits::SD_PATH_LEN],
                    uint16_t maxCount, uint16_t& outCount);

  /** @brief 讀取單筆 session（drillDef 可為 nullptr）*/
  bool loadSession(const char* path, GameRecord& record,
                   DrillDef* drillDef = nullptr);

  uint16_t getSessionCount();
  uint32_t getSDFreeKB();

  float getBestTotal();
  float getBestAvg();
  void  resetBest();

} // namespace SessionMgr
