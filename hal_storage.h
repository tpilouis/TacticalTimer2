/**
 * @file    hal_storage.h
 * @brief   TACTICAL TIMER 2 — NVS + SD 卡儲存 HAL 宣告
 *
 * 職責：
 *  NVS（Non-Volatile Storage）：
 *   - 持久化設定（HitSource、micThresh、preset index、par time 等）
 *   - 遵循 Repository pattern：load() / save() 成對，caller 不需知道底層
 *
 *  SD 卡：
 *   - Session history 序列化為 JSON 存入 SD（目錄 /sessions/）
 *   - 提供 session 列表查詢（供 History mode 顯示）
 *   - 提供單筆 session 讀取（供 History detail 頁面）
 *   - SD 與 Audio(ESP8266Audio) 共用 SPI bus，
 *     SD 操作必須在 AudioTask 不活躍時進行，
 *     或由 UI Task 呼叫（UI Task 與 AudioTask 在不同 Core，
 *     但 SPI bus 需 Mutex 保護 → 由 stateMutex 代理，
 *     或在已確認 audio 停止後操作）
 *
 * 檔案命名規則（SD）：
 *   /sessions/YYYYMMDD_HHmmss_MODE.json
 *   e.g. /sessions/20250322_143500_FREE.json
 *
 * NVS 命名空間：NVS::NVS_NS = "tt2_cfg"（見 config.h）
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"


// ============================================================
// [ST1] App Settings 結構（對應 NVS 完整欄位）
// ============================================================

/**
 * @brief 應用程式設定，對應 NVS 所有儲存項目
 *
 * 使用 HalStorage::loadSettings() / saveSettings() 讀寫。
 */
struct AppSettings {
  HitSource hitSource     = HitSource::MIC;
  int16_t   micThresh     = MicCfg::THRESH_DEF;
  uint8_t   activePreset  = 0;       ///< 0–(PRESET_MAX-1)
  bool      randomDelay   = false;   ///< true = 隨機延遲（Free / RO 共用）
  uint32_t  parTimeMs     = 0;       ///< Free mode Par Time（ms），0 = 停用
  uint32_t  dryBeatMs     = Timing::DRY_BEAT_DEF_MS;
  char      timezone[16]  = "CST-8"; ///< POSIX TZ
  // Drill 課題設定（獨立於 Free Par Time）
  uint8_t   drillShots    = 6;       ///< Drill 目標發數
  uint32_t  drillParMs    = 0;       ///< Drill Par Time（ms），0 = 不限
  uint8_t   drillPassPct  = 80;      ///< Drill 及格比例（0–100）
  uint8_t   maxShots      = 0;       ///< Free/RO 最大發數（0=無限，環形buffer；1-50=到達後自動停）
};


/// 儲存 session 時的額外資訊（選填）
struct SessionExtra {
  const char* presetName = nullptr;  ///< 槍型名稱
  uint32_t    drawTimeMs = 0;        ///< RO 拔槍時間（ms）
  float       drillScore = 0.0f;     ///< DRILL 達成率（%）
  bool        drillPassed= false;    ///< DRILL 是否通過
};

namespace HalStorage {

  // ──────────────────────────────────────────────────────────
  // [ST2] NVS 設定讀寫
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 從 NVS 載入應用程式設定
   *
   * 若 NVS 中無對應值，填入 AppSettings 預設值（首次開機）。
   *
   * @param out  輸出設定
   * @return true  = 成功開啟 NVS
   * @return false = NVS 開啟失敗
   */
  bool loadSettings(AppSettings& out);

  /**
   * @brief 將應用程式設定寫入 NVS
   *
   * @param in   要儲存的設定
   * @return true  = 成功
   * @return false = NVS 寫入失敗
   */
  bool saveSettings(const AppSettings& in);

  /**
   * @brief 從 NVS 載入指定 Preset
   *
   * @param idx    Preset 索引（0–PRESET_MAX-1）
   * @param out    輸出 Preset
   * @return true  = 成功，out 已填入
   * @return false = 不存在或讀取失敗（out 填入預設值）
   */
  bool loadPreset(uint8_t idx, Preset& out);

  /**
   * @brief 將 Preset 寫入 NVS
   *
   * @param idx  Preset 索引（0–PRESET_MAX-1）
   * @param in   要儲存的 Preset
   * @return true  = 成功
   */
  bool savePreset(uint8_t idx, const Preset& in);

  /**
   * @brief 初始化預設 Preset（首次開機時填入 factory defaults）
   *
   * 預設 5 組：Air Pistol / Air Rifle / Pistol / Rifle / Custom
   */
  void initDefaultPresets();


  // ──────────────────────────────────────────────────────────
  // [ST3] SD 卡 Session 儲存
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 初始化 SD 卡（掛載並建立 /sessions/ 目錄）
   *
   * @return true  = SD 卡正常
   * @return false = 掛載失敗（無 SD 卡或格式錯誤）
   */
  bool initSD();

  /**
   * @brief 確認 SD 卡是否已掛載
   * @return true = 已掛載且可用
   */
  bool isSDMounted();

  /**
   * @brief 將 GameRecord 序列化為 JSON 寫入 SD
   *
   * 檔名由時間戳自動生成：/sessions/YYYYMMDD_HHmmss_MODE.json
   *
   * JSON 結構：
   * {
   *   "ts": "2025-03-22T14:35:07",
   *   "mode": "FREE",
   *   "shots": 6,
   *   "totalMs": 4823,
   *   "avgSplitMs": 804,
   *   "hits": [
   *     {"idx":1, "src":0, "elapsedMs":1823, "splitMs":1823},
   *     ...
   *   ]
   * }
   *
   * @param record  要儲存的局紀錄
   * @param mode    本局模式（用於檔名和 JSON 的 mode 欄位）
   * @param pathOut 輸出：實際儲存路徑（可為 nullptr）
   * @return true  = 成功
   * @return false = SD 未掛載或寫入失敗
   */
  bool saveSession(const GameRecord& record, AppMode mode,
                   char* pathOut = nullptr, size_t pathLen = 0,
                   const DrillDef* drillDef = nullptr,
                   const SessionExtra* extra = nullptr);

  /// 從 SD 讀取 session 的額外欄位（preset/drawMs/score/passed）
  bool readSessionExtra(const char* path, char* presetOut, size_t presetLen,
                        uint32_t& drawMs, float& score, bool& passed);

  /**
   * @brief 取得 SD 卡上的 session 列表（按時間排序，最新在前）
   *
   * @param paths     輸出：路徑陣列（呼叫方分配）
   * @param maxCount  陣列最大容量
   * @param outCount  實際填入數量
   * @return true  = 成功
   * @return false = SD 未掛載或目錄不存在
   */
  bool listSessions(char paths[][Limits::SD_PATH_LEN],
                    uint16_t maxCount, uint16_t& outCount);

  /**
   * @brief 從 SD 卡讀取一筆 session JSON，還原為 GameRecord
   *
   * @param path      SD 卡路徑
   * @param record    輸出：還原的 GameRecord
   * @param drillDef  輸出：Drill 課題定義（非 DRILL mode 時為 zero-init）
   * @return true  = 成功
   */
  bool loadSession(const char* path, GameRecord& record,
                   DrillDef* drillDef = nullptr);

  /**
   * @brief 刪除指定 session 檔案
   * @param path  SD 卡完整路徑（e.g. /sessions/xxx.json）
   * @return true = 成功，false = 失敗
   */
  bool deleteSession(const char* path);

  /**
   * @brief 刪除最舊的 session（當 /sessions/ 超過 HISTORY_MAX 筆時）
   * @return 刪除的筆數
   */
  uint16_t pruneOldSessions();


  // ──────────────────────────────────────────────────────────
  // [ST4] 狀態查詢
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 取得 SD 卡剩餘空間（KB）
   * @return 0 = SD 未掛載或查詢失敗
   */
  uint32_t getSDFreeKB();

  /**
   * @brief 取得目前 session 總數
   */
  uint16_t getSessionCount();

} // namespace HalStorage
