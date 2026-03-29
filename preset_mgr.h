/**
 * @file    preset_mgr.h
 * @brief   TACTICAL TIMER 2 — Preset / 槍型管理服務宣告
 *
 * 職責：
 *  - 管理最多 PRESET_MAX 組的 Preset（槍型名稱 + 靈敏度設定）
 *  - 提供 active preset 切換與載入（NVS 持久化）
 *  - Prototype pattern：clone() 複製 preset 為新 preset
 *  - 提供 AppSettings 整合介面（一次 apply 所有設定到 MicEngine）
 *
 * 設計模式：Prototype（clone preset）+ Repository（NVS CRUD）
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include "hal_storage.h"
#include "mic_engine.h"


namespace PresetMgr {

  // ──────────────────────────────────────────────────────────
  // [P1] 生命週期
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 初始化 Preset 管理器
   *
   * 流程：
   *  1. HalStorage::initDefaultPresets()（首次開機寫入出廠預設）
   *  2. 載入 AppSettings 中的 activePreset 索引
   *  3. 載入 active preset 並套用至 MicEngine
   *
   * @param settings  已從 NVS 載入的 AppSettings
   */
  void init(const AppSettings& settings);


  // ──────────────────────────────────────────────────────────
  // [P2] Active Preset 操作
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 切換 active preset
   *
   * 載入指定 preset，套用至 MicEngine，更新 NVS activePreset 索引。
   *
   * @param idx  0–(PRESET_MAX-1)
   * @return true = 成功
   */
  bool setActive(uint8_t idx);

  /**
   * @brief 取得目前 active preset 索引
   */
  uint8_t getActiveIdx();

  /**
   * @brief 取得目前 active preset（只讀參考）
   */
  const Preset& getActive();


  // ──────────────────────────────────────────────────────────
  // [P3] Preset CRUD
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 取得指定 preset（只讀）
   *
   * @param idx  0–(PRESET_MAX-1)
   * @param out  輸出參數
   * @return true = 成功；false = 索引無效（out 填入安全預設）
   */
  bool get(uint8_t idx, Preset& out);

  /**
   * @brief 更新並儲存指定 preset
   *
   * @param idx  0–(PRESET_MAX-1)
   * @param in   新的 preset 內容
   * @return true = 成功
   */
  bool save(uint8_t idx, const Preset& in);

  /**
   * @brief Prototype：複製指定 preset 到另一個 slot
   *
   * @param srcIdx   來源索引
   * @param dstIdx   目標索引（將覆蓋）
   * @param newName  新名稱（nullptr = 保留原名加 " copy"）
   * @return true = 成功
   */
  bool clone(uint8_t srcIdx, uint8_t dstIdx, const char* newName = nullptr);

  /**
   * @brief 重設指定 preset 為出廠預設
   *
   * @param idx  0–(PRESET_MAX-1)
   * @return true = 成功
   */
  bool resetToDefault(uint8_t idx);


  // ──────────────────────────────────────────────────────────
  // [P4] 設定整合
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 將完整 AppSettings 套用至所有相關 Service
   *
   * 更新：
   *  - MicEngine::setConfig()（threshold、cooldown、source）
   *  - TimerCore::setRandomDelay()
   *  - TimerCore::setParTime()
   *
   * @param settings  已載入的 AppSettings
   */
  void applySettings(const AppSettings& settings);

  /**
   * @brief 取得目前生效的 AppSettings 快照（含 active preset 的值）
   */
  AppSettings getEffectiveSettings();

} // namespace PresetMgr
