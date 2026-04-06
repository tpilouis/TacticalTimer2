/**
 * @file    preset_mgr.cpp
 * @brief   TACTICAL TIMER 2 — Preset / 槍型管理服務實作
 *
 * 管理 5 個 Preset（槍型設定組）的 RAM 快取、NVS 持久化，
 * 以及 AppSettings 套用到 MicEngine / TimerCore 的整合邏輯。
 *
 * @version 2.0.0
 */

#include "preset_mgr.h"
#include <Arduino.h>
#include <cstring>
#include "timer_core.h"


// ============================================================
// [P0] 模組內部狀態
// ============================================================
static uint8_t     s_activeIdx = 0;        ///< 目前 active preset 索引（0–4）
static Preset      s_active;               ///< active preset 的 RAM 快取
static AppSettings s_settings;             ///< 目前生效的 AppSettings 快照


// ============================================================
// [P1-impl] 生命週期
// ============================================================

/**
 * @brief 初始化 PresetMgr，載入 active preset 並套用設定
 *
 * 流程：
 *  1. 確保出廠預設已寫入 NVS（HalStorage::initDefaultPresets）
 *  2. 從 settings.activePreset 載入對應 Preset 到 RAM 快取
 *  3. 呼叫 applySettings() 套用到 MicEngine / TimerCore
 *
 * @param settings  從 NVS 載入的 AppSettings
 */
void PresetMgr::init(const AppSettings& settings) {
  s_settings = settings;

  // 首次開機：確保出廠預設已寫入 NVS
  HalStorage::initDefaultPresets();

  // 載入 active preset
  s_activeIdx = settings.activePreset;
  TT2_BOUNDS(s_activeIdx, Limits::PRESET_MAX, { s_activeIdx = 0; });

  if (!HalStorage::loadPreset(s_activeIdx, s_active)) {
    // 使用出廠預設（loadPreset 已填入）
    Serial.printf("[PresetMgr] Preset %u using factory default: %s\n",
                  s_activeIdx, s_active.name);
  } else {
    Serial.printf("[PresetMgr] Preset %u loaded: %s\n",
                  s_activeIdx, s_active.name);
  }

  // 套用至各 Service
  applySettings(settings);
}


// ============================================================
// [P2-impl] Active Preset 操作
// ============================================================

/**
 * @brief 切換 active preset
 *
 * 從 NVS 載入目標 preset，更新 RAM 快取，
 * 即時套用 threshold / cooldown / source 到 MicEngine，
 * 並持久化 activePreset 索引到 NVS。
 *
 * @param idx  Preset 索引（0–PRESET_MAX-1）
 * @return true   切換成功
 * @return false  idx 超出範圍
 */
bool PresetMgr::setActive(uint8_t idx) {
  TT2_BOUNDS(idx, Limits::PRESET_MAX, return false);

  Preset newPreset;
  HalStorage::loadPreset(idx, newPreset);  // 失敗時用出廠預設，不中止

  s_activeIdx = idx;
  s_active    = newPreset;

  // 套用 threshold / cooldown / source 到 MicEngine
  MicEngine::setConfig(
    s_active.micThresh,
    s_active.cooldownMs,
    s_active.hitSource
  );

  // 更新 NVS activePreset 索引
  s_settings.activePreset = idx;
  s_settings.micThresh    = s_active.micThresh;
  s_settings.hitSource    = s_active.hitSource;
  HalStorage::saveSettings(s_settings);

  Serial.printf("[PresetMgr] Active preset: %u (%s)\n", idx, s_active.name);
  return true;
}

/// @brief 取得目前 active preset 索引（0–PRESET_MAX-1）
uint8_t        PresetMgr::getActiveIdx() { return s_activeIdx; }

/// @brief 取得目前 active preset 的 RAM 快取參考（唯讀）
const Preset&  PresetMgr::getActive()    { return s_active; }


// ============================================================
// [P3-impl] Preset CRUD
// ============================================================

/**
 * @brief 讀取指定 Preset
 *
 * @param idx      Preset 索引（0–PRESET_MAX-1）
 * @param[out] out 讀取結果
 * @return true   成功
 * @return false  idx 超出範圍（out 填入安全預設值）
 */
bool PresetMgr::get(uint8_t idx, Preset& out) {
  TT2_BOUNDS(idx, Limits::PRESET_MAX, {
    // 超出範圍：填入安全預設
    strlcpy(out.name, "Invalid", sizeof(out.name));
    out.micThresh  = MicCfg::THRESH_DEF;
    out.cooldownMs = MicCfg::COOLDOWN_MS;
    out.hitSource  = HitSource::MIC;
    return false;
  });
  HalStorage::loadPreset(idx, out);
  return true;
}

/**
 * @brief 儲存 Preset 到 NVS，若為 active preset 則同步 RAM 快取
 *
 * @param idx  Preset 索引
 * @param in   要儲存的 Preset 資料
 * @return true / false
 */
bool PresetMgr::save(uint8_t idx, const Preset& in) {
  TT2_BOUNDS(idx, Limits::PRESET_MAX, return false);
  bool ok = HalStorage::savePreset(idx, in);
  if (ok && idx == s_activeIdx) {
    // active preset 被更新 → 同步 RAM 快取並套用
    s_active = in;
    MicEngine::setConfig(s_active.micThresh,
                         s_active.cooldownMs,
                         s_active.hitSource);
    Serial.printf("[PresetMgr] Active preset updated: %s\n", in.name);
  }
  return ok;
}

/**
 * @brief 複製 Preset 到另一個槽位
 *
 * @param srcIdx   來源索引
 * @param dstIdx   目標索引
 * @param newName  新名稱（nullptr 或空字串 = 原名稱 + " copy"）
 * @return true / false
 */
bool PresetMgr::clone(uint8_t srcIdx, uint8_t dstIdx, const char* newName) {
  TT2_BOUNDS(srcIdx, Limits::PRESET_MAX, return false);
  TT2_BOUNDS(dstIdx, Limits::PRESET_MAX, return false);

  Preset src;
  HalStorage::loadPreset(srcIdx, src);

  // 產生目標名稱
  if (newName && newName[0] != '\0') {
    strlcpy(src.name, newName, sizeof(src.name));
  } else {
    // 原名稱 + " copy"（截斷確保不超長）
    char tmp[Limits::NAME_LEN];
    strlcpy(tmp, src.name, sizeof(tmp));
    snprintf(src.name, sizeof(src.name), "%.10s copy", tmp);
  }

  Serial.printf("[PresetMgr] Clone: %u -> %u (%s)\n",
                srcIdx, dstIdx, src.name);
  return HalStorage::savePreset(dstIdx, src);
}

/**
 * @brief 將指定 Preset 重置為出廠預設值
 *
 * 從 HalStorage 取得出廠預設後儲存，若為 active preset 則即時套用。
 *
 * @param idx  Preset 索引
 * @return true / false
 */
bool PresetMgr::resetToDefault(uint8_t idx) {
  TT2_BOUNDS(idx, Limits::PRESET_MAX, return false);

  // 清除 NVS 中該 preset（讓 loadPreset 回傳出廠預設）
  Preset p;
  HalStorage::loadPreset(idx, p);  // 先讀出廠預設（因為 NVS 空白）
  bool ok = HalStorage::savePreset(idx, p);

  if (ok && idx == s_activeIdx) {
    s_active = p;
    MicEngine::setConfig(s_active.micThresh,
                         s_active.cooldownMs,
                         s_active.hitSource);
  }
  Serial.printf("[PresetMgr] Preset %u reset to default: %s\n", idx, p.name);
  return ok;
}


// ============================================================
// [P4-impl] 設定整合
// ============================================================

/**
 * @brief 套用 AppSettings 到 MicEngine 和 TimerCore
 *
 * 優先順序：
 *  - threshold / source：Active Preset 有值時優先，否則用 AppSettings 全域值
 *  - cooldown：Active Preset 有值時優先，否則用 MicCfg::COOLDOWN_MS
 *  - randomDelay / parTimeMs：直接套用 AppSettings 值
 *
 * @param settings  要套用的 AppSettings
 */
void PresetMgr::applySettings(const AppSettings& settings) {
  s_settings = settings;

  // MicEngine：使用 active preset 的 threshold（覆蓋 AppSettings 的全域值）
  // 若 active preset 有效則優先，否則使用 AppSettings.micThresh
  int16_t  thresh    = s_active.micThresh  != 0
                       ? s_active.micThresh : settings.micThresh;
  uint32_t cooldown  = s_active.cooldownMs != 0
                       ? s_active.cooldownMs : MicCfg::COOLDOWN_MS;
  HitSource source   = (s_active.name[0] != '\0')
                       ? s_active.hitSource : settings.hitSource;

  MicEngine::setConfig(thresh, cooldown, source);

  // TimerCore：隨機延遲 + par time
  TimerCore::setRandomDelay(settings.randomDelay);
  TimerCore::setParTime(settings.parTimeMs);

  Serial.printf("[PresetMgr] Settings applied: thresh=%d src=%d rand=%d par=%ums\n",
                thresh, (int)source,
                (int)settings.randomDelay, settings.parTimeMs);
}

/**
 * @brief 取得目前生效的 AppSettings（以 active preset 值覆蓋全域值）
 *
 * 回傳值反映實際生效的 micThresh / hitSource / activePreset，
 * 而非 NVS 中儲存的全域預設值。供 SETTINGS 頁面初始化使用。
 *
 * @return 合併後的 AppSettings 快照
 */
AppSettings PresetMgr::getEffectiveSettings() {
  AppSettings eff = s_settings;
  // 以 active preset 值覆蓋
  if (s_active.name[0] != '\0') {
    eff.micThresh  = s_active.micThresh;
    eff.hitSource  = s_active.hitSource;
    eff.activePreset = s_activeIdx;
  }
  return eff;
}
