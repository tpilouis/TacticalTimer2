/**
 * @file    session_mgr.cpp
 * @brief   TACTICAL TIMER 2 — Session 管理服務實作
 *
 * 作為 TimerCore 與 HalStorage 之間的薄層，
 * 負責每局結束時的 session 儲存、最佳紀錄維護，
 * 以及 History 功能所需的列表與詳情讀取。
 *
 * @version 2.0.0
 */

#include "session_mgr.h"
#include <Arduino.h>


/// @cond INTERNAL
/// 最佳紀錄 RAM 快取（重開機歸零，不持久化）
static float s_bestTotal = 0.0f;
static float s_bestAvg   = 0.0f;
/// @endcond


/**
 * @brief 初始化 SessionMgr，掛載 SD 卡
 *
 * @return true   SD 掛載成功
 * @return false  SD 不可用（History 功能停用，但系統繼續運作）
 */
bool SessionMgr::init() {
  bool ok = HalStorage::initSD();
  if (ok) {
    Serial.printf("[SessionMgr] SD OK, sessions=%u, free=%uKB\n",
                  HalStorage::getSessionCount(),
                  HalStorage::getSDFreeKB());
  } else {
    Serial.println("[SessionMgr] SD not available, history disabled");
  }
  return ok;
}

/**
 * @brief 儲存最近一局（從 TimerCore::getRecord(0) 取得）
 *
 * 跳過 hit_count == 0 的空局。
 * 儲存成功後更新 TimerCore 的最佳紀錄，並觸發 onBestUpdate 回呼。
 *
 * @param mode          本局 AppMode（FREE / DRILL / RO / ...）
 * @param onBestUpdate  最佳更新回呼（bestTotal, bestAvg），nullptr = 略過
 * @param extra         額外資訊（presetName / drawTimeMs / drillScore），nullptr = 略過
 * @return true   儲存成功
 * @return false  無紀錄、無命中或 SD 寫入失敗
 */
bool SessionMgr::saveLastSession(AppMode mode,
                                 void (*onBestUpdate)(float, float),
                                 const SessionExtra* extra) {
  const GameRecord* rec = TimerCore::getRecord(0);
  if (!rec) {
    Serial.println("[SessionMgr] saveLastSession: no record");
    return false;
  }
  if (rec->hit_count == 0) {
    Serial.println("[SessionMgr] saveLastSession: no hits, skipping");
    return false;
  }

  // 計算本局並更新最佳
  unsigned long totalMs = TimerCore::calcTotalMs(*rec);
  if (totalMs > 0 && rec->hit_count > 0) {
    float totalSec = totalMs / 1000.0f;
    float avgSec   = totalSec / rec->hit_count;
    TimerCore::updateBest(totalSec, avgSec);
    if (onBestUpdate) {
      onBestUpdate(TimerCore::getBestTotal(), TimerCore::getBestAvg());
    }
  }

  return saveSession(*rec, mode, nullptr, extra);
}

/**
 * @brief 儲存指定 GameRecord 到 SD
 *
 * 供 DRILL mode 使用（傳入 DrillDef）。
 *
 * @param record    GameRecord 資料
 * @param mode      AppMode
 * @param drillDef  DRILL 課題定義（nullptr = 不寫入 drill 區段）
 * @param extra     額外資訊（nullptr = 略過）
 * @return true / false
 */
bool SessionMgr::saveSession(const GameRecord& record, AppMode mode,
                             const DrillDef* drillDef,
                             const SessionExtra* extra) {
  if (!HalStorage::isSDMounted()) {
    Panic::warn(TT2Error::SD_WRITE_FAIL, "SessionMgr: SD not mounted");
    return false;
  }
  return HalStorage::saveSession(record, mode, nullptr, 0, drillDef, extra);
}

/**
 * @brief 列出 SD 卡上最新的 maxCount 筆 session 路徑
 *
 * 收集全部檔案 → 字串排序（= 時間排序）→ 取最新 N 筆。
 *
 * @param[out] paths    輸出路徑陣列
 * @param maxCount      最多返回筆數
 * @param[out] outCount 實際返回筆數
 * @return true / false
 */
bool SessionMgr::listSessions(char paths[][Limits::SD_PATH_LEN],
                               uint16_t maxCount, uint16_t& outCount) {
  return HalStorage::listSessions(paths, maxCount, outCount);
}

/**
 * @brief 讀取單筆 session JSON 並填入 GameRecord
 *
 * @param path          SD 卡絕對路徑
 * @param[out] record   讀取結果
 * @param[out] drillDef DRILL 課題定義輸出（nullptr = 不讀取）
 * @return true / false
 */
bool SessionMgr::loadSession(const char* path, GameRecord& record,
                             DrillDef* drillDef) {
  return HalStorage::loadSession(path, record, drillDef);
}

/// @brief 取得 SD 卡上的 session 總筆數
uint16_t SessionMgr::getSessionCount() { return HalStorage::getSessionCount(); }

/// @brief 取得 SD 卡剩餘空間（KB）
uint32_t SessionMgr::getSDFreeKB()     { return HalStorage::getSDFreeKB(); }

/// @brief 取得歷史最佳總時間（秒），從 TimerCore 讀取
float    SessionMgr::getBestTotal()    { return TimerCore::getBestTotal(); }

/// @brief 取得歷史最佳平均 split（秒），從 TimerCore 讀取
float    SessionMgr::getBestAvg()      { return TimerCore::getBestAvg(); }

/**
 * @brief 重置本次開機的最佳紀錄顯示
 *
 * @note 僅影響本次開機的 Web 顯示，不清除 TimerCore 的內部最佳值，
 *       也不影響 SD 卡歷史記錄。
 */
void     SessionMgr::resetBest() {
  // TimerCore 沒有 reset API，用 0 覆蓋（透過 updateBest 無法清零）
  // 直接呼叫 TimerCore::init() 會重置所有狀態，太激進
  // 暫時保留 session_mgr 自己的 reset（只影響 Web display）
  s_bestTotal = 0.0f;
  s_bestAvg   = 0.0f;
  Serial.println("[SessionMgr] Best records reset");
}
