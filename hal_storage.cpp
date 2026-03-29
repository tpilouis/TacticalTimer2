/**
 * @file    hal_storage.cpp
 * @brief   TACTICAL TIMER 2 — NVS + SD 卡儲存 HAL 實作
 *
 * 實作兩類儲存：
 *  1. NVS（Non-Volatile Storage）：AppSettings、5 個 Preset、版本號
 *  2. SD 卡：Session JSON 檔案（/sessions/YYYYMMDD_HHMMSS_MODE.json）
 *
 * Preset 以二進位序列化（getBytes/putBytes）儲存，避免多次 NVS 讀寫。
 * Session JSON 包含時間戳、命中紀錄、DrillDef 和 SessionExtra 等欄位。
 *
 * @version 2.0.0
 */

#include "hal_storage.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <ArduinoJson.h>


// ============================================================
// [ST0] 模組內部狀態
// ============================================================
static bool       s_sdMounted   = false;
static uint16_t   s_sessionCount = 0;  ///< 快取 session 數量

static constexpr char SESSION_DIR[] = "/sessions";

/// Preset NVS key 前綴（"preset_0" … "preset_4"）
static constexpr char PRESET_KEY_PREFIX[] = "preset_";

/// Preset name fallbacks（首次開機使用）
static const char* DEFAULT_PRESET_NAMES[Limits::PRESET_MAX] = {
  "Air Pistol",
  "Air Rifle",
  "Pistol",
  "Rifle",
  "Custom"
};

static const int16_t DEFAULT_PRESET_THRESH[Limits::PRESET_MAX] = {
  1700,   // Air Pistol：相對安靜
  1500,   // Air Rifle：更安靜
  3000,   // Pistol：較響
  2500,   // Rifle：較響
  MicCfg::THRESH_DEF  // Custom：預設值
};


// ============================================================
// [ST2-impl] NVS 設定讀寫
// ============================================================

/**
 * @brief 從 NVS 讀取 AppSettings
 *
 * 讀取失敗時（NVS namespace 不存在）回傳 false，
 * out 保持呼叫方傳入的初始值（通常為零值）。
 *
 * @param[out] out  讀取結果
 * @return true   讀取成功
 * @return false  NVS 開啟失敗
 */
bool HalStorage::loadSettings(AppSettings& out) {
  Preferences prefs;
  if (!prefs.begin(NVS::NVS_NS, /*readOnly=*/true)) {
    Serial.println("[HalStorage] NVS open failed (read), using defaults");
    Panic::warn(TT2Error::NVS_OPEN_FAIL, "loadSettings");
    return false;
  }

  out.hitSource    = static_cast<HitSource>(
                       prefs.getUChar(NVS::HIT_SRC,
                         static_cast<uint8_t>(HitSource::MIC)));
  out.micThresh    = static_cast<int16_t>(
                       prefs.getShort(NVS::MIC_THRESH, MicCfg::THRESH_DEF));
  out.activePreset = prefs.getUChar(NVS::ACTIVE_PRESET, 0);
  out.randomDelay  = prefs.getBool(NVS::RAND_DELAY, false);
  out.parTimeMs    = prefs.getULong(NVS::PAR_TIME_MS, 0);
  out.dryBeatMs    = prefs.getULong(NVS::DRY_BEAT_MS, Timing::DRY_BEAT_DEF_MS);
  out.drillShots   = prefs.getUChar(NVS::DRILL_SHOTS,    6);
  out.drillParMs   = prefs.getULong(NVS::DRILL_PAR_MS,   0);
  out.drillPassPct = prefs.getUChar(NVS::DRILL_PASS_PCT, 80);
  prefs.getString(NVS::TZ_KEY, out.timezone, sizeof(out.timezone));
  if (out.timezone[0] == '\0') {
    strlcpy(out.timezone, NetCfg::NTP_TZ, sizeof(out.timezone));
  }

  prefs.end();

  Serial.printf("[HalStorage] Settings loaded: src=%d thr=%d preset=%d rand=%d\n",
                (int)out.hitSource, out.micThresh,
                out.activePreset, (int)out.randomDelay);
  return true;
}

/**
 * @brief 將 AppSettings 寫入 NVS
 * @param in  要儲存的設定值
 * @return true / false
 */
bool HalStorage::saveSettings(const AppSettings& in) {
  Preferences prefs;
  if (!prefs.begin(NVS::NVS_NS, /*readOnly=*/false)) {
    Panic::warn(TT2Error::NVS_OPEN_FAIL, "saveSettings");
    return false;
  }

  prefs.putUChar(NVS::HIT_SRC,       static_cast<uint8_t>(in.hitSource));
  prefs.putShort(NVS::MIC_THRESH,    in.micThresh);
  prefs.putUChar(NVS::ACTIVE_PRESET, in.activePreset);
  prefs.putBool( NVS::RAND_DELAY,    in.randomDelay);
  prefs.putULong(NVS::PAR_TIME_MS,   in.parTimeMs);
  prefs.putULong(NVS::DRY_BEAT_MS,   in.dryBeatMs);
  prefs.putUChar(NVS::DRILL_SHOTS,    in.drillShots);
  prefs.putULong(NVS::DRILL_PAR_MS,   in.drillParMs);
  prefs.putUChar(NVS::DRILL_PASS_PCT, in.drillPassPct);
  prefs.putString(NVS::TZ_KEY,     in.timezone);
  prefs.end();

  Serial.println("[HalStorage] Settings saved to NVS");
  return true;
}

/**
 * @brief 從 NVS 讀取指定 Preset
 *
 * 以 getBytes 讀取二進位序列化的 Preset。
 * 若尚未儲存（首次開機）則填入對應的出廠預設值並回傳 false。
 *
 * @param idx      Preset 索引（0–PRESET_MAX-1）
 * @param[out] out 讀取結果（失敗時填入出廠預設）
 * @return true   從 NVS 成功讀取
 * @return false  使用出廠預設（idx 超範圍或 NVS 無資料）
 */
bool HalStorage::loadPreset(uint8_t idx, Preset& out) {
  TT2_BOUNDS(idx, Limits::PRESET_MAX, {
    // 超出範圍：填入安全預設
    strlcpy(out.name, "Default", sizeof(out.name));
    out.hitSource  = HitSource::MIC;
    out.micThresh  = MicCfg::THRESH_DEF;
    out.cooldownMs = MicCfg::COOLDOWN_MS;
    return false;
  });

  char key[24];
  snprintf(key, sizeof(key), "%s%u", PRESET_KEY_PREFIX, (unsigned)idx);

  Preferences prefs;
  if (!prefs.begin(NVS::NVS_NS, /*readOnly=*/true)) {
    Panic::warn(TT2Error::NVS_OPEN_FAIL, "loadPreset");
    return false;
  }

  // Preset 存為 bytes（二進位序列化，避免多次 NVS 讀寫）
  size_t loaded = prefs.getBytes(key, &out, sizeof(Preset));
  prefs.end();

  if (loaded != sizeof(Preset)) {
    // 尚未儲存過：填入對應的出廠預設
    strlcpy(out.name, DEFAULT_PRESET_NAMES[idx], sizeof(out.name));
    out.hitSource  = HitSource::MIC;
    out.micThresh  = DEFAULT_PRESET_THRESH[idx];
    out.cooldownMs = MicCfg::COOLDOWN_MS;
    return false;  // 回傳 false 表示使用了 fallback
  }

  return true;
}

/**
 * @brief 將 Preset 寫入 NVS（以 putBytes 二進位序列化）
 * @param idx  Preset 索引（0–PRESET_MAX-1）
 * @param in   要儲存的 Preset
 * @return true / false
 */
bool HalStorage::savePreset(uint8_t idx, const Preset& in) {
  TT2_BOUNDS(idx, Limits::PRESET_MAX, return false);

  char key[24];
  snprintf(key, sizeof(key), "%s%u", PRESET_KEY_PREFIX, (unsigned)idx);

  Preferences prefs;
  if (!prefs.begin(NVS::NVS_NS, /*readOnly=*/false)) {
    Panic::warn(TT2Error::NVS_OPEN_FAIL, "savePreset");
    return false;
  }

  size_t written = prefs.putBytes(key, &in, sizeof(Preset));
  prefs.end();

  if (written != sizeof(Preset)) {
    Panic::warn(TT2Error::NVS_WRITE_FAIL, "savePreset: size mismatch");
    return false;
  }
  return true;
}

/**
 * @brief 初始化出廠預設 Preset（首次開機或格式版本升級時呼叫）
 *
 * 流程：
 *  1. 讀取 NVS 版本號，若不符（格式升級）則清除所有 preset NVS 資料
 *  2. 逐一檢查 5 個 Preset：若 NVS 無資料則寫入出廠預設
 *
 * @note 必須在 PresetMgr::init() 之前呼叫
 */
void HalStorage::initDefaultPresets() {
  Preferences prefs;

  // ── 版本號檢查：偵測舊格式或首次開機 ──────────────────────
  // 若 NVS 裡的版本號不符，清除所有 Preset 資料，強制重新寫入
  uint8_t storedVer = 0;
  if (prefs.begin(NVS::NVS_NS, true)) {
    storedVer = prefs.getUChar(NVS::FMT_VER, 0);
    prefs.end();
  }

  if (storedVer != NVS::FMT_VER_VAL) {
    Serial.printf("[HalStorage] NVS format mismatch (stored=%u, expected=%u)\n",
                  storedVer, NVS::FMT_VER_VAL);
    Serial.println("[HalStorage] Clearing all preset NVS data...");

    // 清除所有 preset 鍵值
    if (prefs.begin(NVS::NVS_NS, false)) {
      char key[24];
      for (uint8_t i = 0; i < Limits::PRESET_MAX; i++) {
        snprintf(key, sizeof(key), "%s%u", PRESET_KEY_PREFIX, (unsigned)i);
        prefs.remove(key);
      }
      // 寫入新版本號
      prefs.putUChar(NVS::FMT_VER, NVS::FMT_VER_VAL);
      prefs.end();
    }
    Serial.println("[HalStorage] NVS preset data cleared, will reinitialize.");
  }

  // ── 初始化（若某個 Preset 不存在則寫入出廠預設）──────────
  for (uint8_t i = 0; i < Limits::PRESET_MAX; i++) {
    Preset p;
    if (!loadPreset(i, p)) {
      // loadPreset 已填入出廠預設，直接存回 NVS
      savePreset(i, p);
      Serial.printf("[HalStorage] Default preset %u ('%s') thresh=%d src=%d\n",
                    i, p.name, p.micThresh, (int)p.hitSource);
    } else {
      Serial.printf("[HalStorage] Preset %u OK: '%s' thresh=%d src=%d\n",
                    i, p.name, p.micThresh, (int)p.hitSource);
    }
  }
}


// ============================================================
// [ST3-impl] SD 卡 Session 儲存
// ============================================================

/**
 * @brief 初始化 SD 卡並建立 sessions 目錄
 *
 * M5Core2 SD 使用預設 SPI CS = GPIO 4。
 * 掛載後快取 session 數量（getSessionCount）。
 *
 * @return true   SD 掛載成功
 * @return false  SD 不存在或掛載失敗
 */
bool HalStorage::initSD() {
  // M5Core2 SD 使用預設 SPI CS = GPIO 4
  if (!SD.begin(4)) {
    Serial.println("[HalStorage] SD mount failed");
    Panic::warn(TT2Error::SD_MOUNT_FAIL, "initSD");
    s_sdMounted = false;
    return false;
  }
  s_sdMounted = true;
  Serial.printf("[HalStorage] SD mounted: %lluMB total\n",
                SD.totalBytes() / (1024 * 1024));

  // 建立 sessions 目錄（若不存在）
  if (!SD.exists(SESSION_DIR)) {
    SD.mkdir(SESSION_DIR);
    Serial.printf("[HalStorage] Created directory: %s\n", SESSION_DIR);
  }

  // 快取 session 數量
  s_sessionCount = getSessionCount();
  Serial.printf("[HalStorage] Sessions found: %u\n", s_sessionCount);
  return true;
}

/// @brief 查詢 SD 卡是否已掛載
/// @return true 已掛載，可進行 Session 讀寫
bool HalStorage::isSDMounted() {
  return s_sdMounted;
}

/**
 * @brief 儲存 session 到 SD 卡 JSON 檔案
 *
 * 檔名格式：`/sessions/YYYYMMDD_HHMMSS_MODE.json`
 *
 * JSON 包含：
 *  - `ts`：時間戳（ISO 8601）
 *  - `mode`：模式字串（FREE / DRILL / RO / SPY）
 *  - `shots`：命中數
 *  - `totalMs`：總時間（ms）
 *  - `avgSplitMs`：平均分段時間
 *  - `hits[]`：各槍詳細資料（elapsedMs / splitMs / src）
 *  - `preset`：槍型名稱（若 extra 提供）
 *  - `drawMs`：RO 拔槍時間（若 extra 提供）
 *  - `score` / `passed`：DRILL 達成率（若 extra 提供）
 *  - `drill`：DrillDef（DRILL mode 專用）
 *
 * @param record   GameRecord
 * @param mode     AppMode
 * @param pathOut  輸出儲存路徑（nullptr = 不輸出）
 * @param pathLen  pathOut 緩衝區長度
 * @param drillDef DRILL 課題定義（nullptr = 不寫 drill 區段）
 * @param extra    額外資訊（nullptr = 略過）
 * @return true / false
 */
bool HalStorage::saveSession(const GameRecord& record, AppMode mode,
                             char* pathOut, size_t pathLen,
                             const DrillDef* drillDef,
                             const SessionExtra* extra) {
  if (!s_sdMounted) {
    Panic::warn(TT2Error::SD_WRITE_FAIL, "saveSession: SD not mounted");
    return false;
  }

  // 建立檔名（時間戳 + 模式）
  time_t now = time(nullptr);
  struct tm* lt = localtime(&now);

  const char* modeStr = "UNK";
  switch (mode) {
    case AppMode::FREE:     modeStr = "FREE";  break;
    case AppMode::DRILL:    modeStr = "DRILL"; break;
    case AppMode::DRY_FIRE: modeStr = "DRY";   break;
    case AppMode::SPY:      modeStr = "SPY";   break;
    case AppMode::RO:       modeStr = "RO";    break;
    default: break;
  }

  char path[Limits::SD_PATH_LEN];
  snprintf(path, sizeof(path), "%s/%04d%02d%02d_%02d%02d%02d_%s.json",
           SESSION_DIR,
           lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
           lt->tm_hour, lt->tm_min, lt->tm_sec,
           modeStr);

  // 序列化為 JSON（含 drill 區段，需要 1024 bytes 容量）
  StaticJsonDocument<1024> doc;

  char tsStr[20];
  snprintf(tsStr, sizeof(tsStr), "%04d-%02d-%02dT%02d:%02d:%02d",
           lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
           lt->tm_hour, lt->tm_min, lt->tm_sec);
  doc["ts"]   = tsStr;
  doc["mode"] = modeStr;
  doc["shots"]= record.hit_count;

  // 額外資訊
  if (extra) {
    if (extra->presetName && extra->presetName[0] != '\0')
      doc["preset"] = extra->presetName;
    if (extra->drawTimeMs > 0)
      doc["drawMs"] = extra->drawTimeMs;
    if (mode == AppMode::DRILL) {
      doc["score"]  = extra->drillScore;
      doc["passed"] = extra->drillPassed ? 1 : 0;
    }
  }

  uint32_t totalMs = 0;
  if (record.hit_count > 0 && record.start_time_ms > 0) {
    totalMs = static_cast<uint32_t>(
      record.hits[record.hit_count - 1].hit_time_ms - record.start_time_ms);
  }
  doc["totalMs"] = totalMs;
  doc["avgSplitMs"] = (record.hit_count > 0) ? (totalMs / record.hit_count) : 0;

  JsonArray hits = doc.createNestedArray("hits");
  for (uint8_t i = 0; i < record.hit_count; i++) {
    TT2_BOUNDS(i, Limits::HIT_MAX, break);
    JsonObject h = hits.createNestedObject();
    h["idx"] = i + 1;
    h["src"] = record.hits[i].station_id;
    unsigned long elapsed = record.hits[i].hit_time_ms - record.start_time_ms;
    unsigned long split   = (i == 0)
      ? elapsed
      : (record.hits[i].hit_time_ms - record.hits[i-1].hit_time_ms);
    h["elapsedMs"] = elapsed;
    h["splitMs"]   = split;
  }

  // Drill 課題定義（DRILL mode 專用）
  if (drillDef && mode == AppMode::DRILL) {
    JsonObject drill = doc.createNestedObject("drill");
    drill["parMs"]   = drillDef->parTimeMs;
    drill["shots"]   = drillDef->shotCount;
    drill["passPct"] = drillDef->passPct;
    drill["name"]    = drillDef->name;
  }

  // 寫入 SD
  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[HalStorage] Cannot open for write: %s\n", path);
    Panic::warn(TT2Error::SD_WRITE_FAIL, path);
    return false;
  }

  size_t written = serializeJson(doc, f);
  f.close();

  if (written == 0) {
    Panic::warn(TT2Error::SD_WRITE_FAIL, "saveSession: serializeJson wrote 0 bytes");
    return false;
  }

  s_sessionCount++;

  if (pathOut && pathLen > 0) {
    strlcpy(pathOut, path, pathLen);
  }

  Serial.printf("[HalStorage] Session saved: %s (%u bytes)\n", path, (unsigned)written);

  // 超過上限時自動清理
  if (s_sessionCount > Limits::HISTORY_MAX) {
    pruneOldSessions();
  }

  return true;
}

/**
 * @brief 從 session JSON 讀取額外欄位（preset / drawMs / score / passed）
 *
 * 使用 StaticJsonDocument<512> 讀取，避免 web_server.cpp 直接操作 SD。
 *
 * @param path       SD 卡路徑
 * @param presetOut  槍型名稱輸出緩衝區
 * @param presetLen  presetOut 長度
 * @param drawMs     RO 拔槍時間輸出
 * @param score      DRILL 達成率輸出
 * @param passed     DRILL 是否通過輸出
 * @return true / false
 */
bool HalStorage::readSessionExtra(const char* path, char* presetOut, size_t presetLen,
                                   uint32_t& drawMs, float& score, bool& passed) {
  presetOut[0] = '\0'; drawMs = 0; score = 0.0f; passed = false;
  if (!s_sdMounted) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  StaticJsonDocument<512> doc;
  bool ok = (deserializeJson(doc, f) == DeserializationError::Ok);
  f.close();
  if (!ok) return false;
  if (doc.containsKey("preset")) strlcpy(presetOut, doc["preset"] | "", presetLen);
  drawMs = doc["drawMs"] | 0;
  score  = doc["score"]  | 0.0f;
  passed = (doc["passed"] | 0) != 0;
  return true;
}

/**
 * @brief 列出 SD 卡上最新的 maxCount 筆 session 路徑
 *
 * 演算法：
 *  1. 收集全部 .json 檔案（最多 200 筆靜態暫存）
 *  2. Bubble sort 字串排序（檔名含時間戳，等同時間排序）
 *  3. 取末尾 maxCount 筆（最新）
 *
 * @param[out] paths    輸出路徑陣列
 * @param maxCount      最多返回筆數
 * @param[out] outCount 實際返回筆數
 * @return true / false
 */
bool HalStorage::listSessions(char paths[][Limits::SD_PATH_LEN],
                              uint16_t maxCount, uint16_t& outCount) {
  outCount = 0;
  if (!s_sdMounted || maxCount == 0) return false;

  File dir = SD.open(SESSION_DIR);
  if (!dir || !dir.isDirectory()) {
    Panic::warn(TT2Error::SD_READ_FAIL, "listSessions: cannot open dir");
    return false;
  }

  // 先收集所有 .json 檔案（靜態暫存，最多 200 筆）
  static char tmp[200][Limits::SD_PATH_LEN];
  uint16_t total = 0;

  while (total < 200) {
    File entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) { entry.close(); continue; }

    const char* name = entry.name();
    size_t nameLen = strlen(name);
    if (nameLen < 5 ||
        strcmp(name + nameLen - 5, ".json") != 0) {
      entry.close();
      continue;
    }
    snprintf(tmp[total], Limits::SD_PATH_LEN, "%s/%s", SESSION_DIR, name);
    total++;
    entry.close();
  }
  dir.close();

  // 檔名格式 YYYYMMDD_HHMMSS_MODE.json，字串排序 = 時間排序
  // 排序（bubble sort，筆數少效能夠用）
  for (uint16_t i = 0; i < total - 1; i++) {
    for (uint16_t j = 0; j < total - 1 - i; j++) {
      if (strcmp(tmp[j], tmp[j+1]) > 0) {
        char swap[Limits::SD_PATH_LEN];
        strlcpy(swap, tmp[j], sizeof(swap));
        strlcpy(tmp[j], tmp[j+1], sizeof(tmp[j]));
        strlcpy(tmp[j+1], swap, sizeof(tmp[j+1]));
      }
    }
  }

  // 取最新 maxCount 筆（排序後末尾是最新）
  uint16_t start = (total > maxCount) ? (total - maxCount) : 0;
  for (uint16_t i = start; i < total && outCount < maxCount; i++) {
    strlcpy(paths[outCount], tmp[i], Limits::SD_PATH_LEN);
    outCount++;
  }

  Serial.printf("[HalStorage] listSessions: total=%d returned=%d\n", total, outCount);
  return true;
}

/**
 * @brief 讀取單筆 session JSON 並填入 GameRecord
 *
 * hit_time_ms 存入 elapsedMs（相對時間），start_time_ms 設為 0。
 *
 * @param path          SD 卡絕對路徑
 * @param[out] record   讀取結果
 * @param[out] drillDef DRILL 課題定義（nullptr = 不讀取）
 * @return true / false
 */
bool HalStorage::loadSession(const char* path, GameRecord& record,
                             DrillDef* drillDef) {
  TT2_NOT_NULL(path, return false);
  if (!s_sdMounted) return false;

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Panic::warn(TT2Error::SD_READ_FAIL, path);
    return false;
  }

  StaticJsonDocument<1024> doc;  // 加大容量容納 drill 區段
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.printf("[HalStorage] JSON parse error: %s\n", err.c_str());
    return false;
  }

  record.hit_count = doc["shots"] | 0;
  TT2_BOUNDS(record.hit_count, Limits::HIT_MAX + 1, {
    record.hit_count = Limits::HIT_MAX;
  });

  record.start_time_ms = 0;
  record.stop_time_ms  = doc["totalMs"] | 0;

  // 讀取 mode
  const char* modeStr = doc["mode"] | "FREE";
  if      (strcmp(modeStr, "DRILL")   == 0) record.mode = AppMode::DRILL;
  else if (strcmp(modeStr, "SPY")     == 0) record.mode = AppMode::SPY;
  else if (strcmp(modeStr, "RO")      == 0) record.mode = AppMode::RO;
  else if (strcmp(modeStr, "HISTORY") == 0) record.mode = AppMode::HISTORY;
  else                                       record.mode = AppMode::FREE;

  JsonArray hits = doc["hits"].as<JsonArray>();
  uint8_t i = 0;
  for (JsonObject h : hits) {
    if (i >= record.hit_count) break;
    record.hits[i].station_id  = h["src"] | 0;
    record.hits[i].hit_time_ms = h["elapsedMs"] | 0;
    i++;
  }

  // 讀取 Drill 課題定義
  if (drillDef) {
    memset(drillDef, 0, sizeof(DrillDef));
    if (doc.containsKey("drill")) {
      JsonObject drill = doc["drill"];
      drillDef->parTimeMs  = drill["parMs"]   | 0;
      drillDef->shotCount  = drill["shots"]   | 0;
      drillDef->passPct    = drill["passPct"] | 80;
      const char* dn = drill["name"] | "";
      strlcpy(drillDef->name, dn, sizeof(drillDef->name));
    }
  }

  return true;
}

/**
 * @brief 刪除指定 session 檔案
 * @param path  SD 卡絕對路徑
 * @return true / false
 */
bool HalStorage::deleteSession(const char* path) {
  if (!s_sdMounted) {
    Serial.println("[HalStorage] deleteSession: SD not mounted");
    return false;
  }
  if (!SD.exists(path)) {
    Serial.printf("[HalStorage] deleteSession: not found: %s\n", path);
    return false;
  }
  bool ok = SD.remove(path);
  Serial.printf("[HalStorage] deleteSession: %s -> %s\n",
                path, ok ? "OK" : "FAIL");
  return ok;
}

/**
 * @brief 自動清理最舊的 session，使總數不超過 HISTORY_MAX
 *
 * 以字串排序找最舊的檔案（時間戳最小）並刪除。
 *
 * @return 實際刪除的筆數
 */
uint16_t HalStorage::pruneOldSessions() {
  if (!s_sdMounted) return 0;

  // 取得所有路徑（靜態陣列，避免動態分配）
  // 最多處理 HISTORY_MAX + 50 筆
  constexpr uint16_t BUF_SIZE = Limits::HISTORY_MAX + 50;
  static char paths[BUF_SIZE][Limits::SD_PATH_LEN];  // static: stack 太大
  uint16_t count = 0;

  if (!listSessions(paths, BUF_SIZE, count)) return 0;

  if (count <= Limits::HISTORY_MAX) return 0;

  // 刪除最舊的（字串排序：最小 = 最舊）
  // 簡易做法：路徑含時間戳，最小字串即最早
  uint16_t toDelete = count - Limits::HISTORY_MAX;
  uint16_t deleted  = 0;

  // 找最舊的 toDelete 筆（線性掃描）
  for (uint16_t d = 0; d < toDelete; d++) {
    uint16_t oldest = 0;
    for (uint16_t i = 1; i < count; i++) {
      if (strcmp(paths[i], paths[oldest]) < 0) oldest = i;
    }
    if (SD.remove(paths[oldest])) {
      deleted++;
      Serial.printf("[HalStorage] Pruned: %s\n", paths[oldest]);
    }
    // 標記為已刪除（填空字串）
    paths[oldest][0] = '\xff';  // 確保排在後面
  }

  s_sessionCount = count - deleted;
  return deleted;
}


// ============================================================
// [ST4-impl] 狀態查詢
// ============================================================

/// @brief 取得 SD 卡剩餘空間（KB）
/// @return 剩餘 KB，SD 未掛載時回傳 0
uint32_t HalStorage::getSDFreeKB() {
  if (!s_sdMounted) return 0;
  return static_cast<uint32_t>(SD.totalBytes() / 1024 -
                                SD.usedBytes()  / 1024);
}

/**
 * @brief 計算並快取 SD 卡上的 session 檔案總數
 *
 * 掃描 /sessions 目錄下所有非目錄檔案。
 *
 * @return session 總數，SD 未掛載時回傳 0
 */
uint16_t HalStorage::getSessionCount() {
  if (!s_sdMounted) return 0;

  File dir = SD.open(SESSION_DIR);
  if (!dir || !dir.isDirectory()) return 0;

  uint16_t count = 0;
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) count++;
    entry.close();
  }
  dir.close();

  s_sessionCount = count;
  return count;
}
