/**
 * @file    mode_history.cpp
 * @brief   TACTICAL TIMER 2 — History 模式實作
 *
 * 提供 Core2 端的 session 瀏覽介面，支援：
 *  - 分頁列表（每頁 PAGE_SIZE 筆）
 *  - 單筆詳情（命中時間、split、DRILL pass/fail）
 *  - 刪除確認（二步驟防誤刪）
 *
 * 三個 Phase：LIST → DETAIL → CONFIRM_DELETE
 *
 * @version 2.0.0
 */

#include "mode_history.h"
#include "game_fsm.h"
#include "hal_storage.h"
#include <M5Core2.h>
#include <Arduino.h>
#include <cstring>

namespace UIScreen {
  void drawModeHeader(const char*, const char*, uint16_t);
  void clearGameArea();
  void drawBtnBar(const char*, const char*, const char*);
  void drawHistoryList(const char paths[][Limits::SD_PATH_LEN],
                       uint8_t count, uint16_t total,
                       uint16_t pageStart, uint32_t freeKB);
  void drawHistoryDetail(const char* path, const GameRecord& rec,
                         const DrillDef* drillDef = nullptr);
}

namespace UIColor {
  constexpr uint16_t TEXT_DIM = 0x5AB0; ///< 次要文字顏色
}

/// @cond INTERNAL
/// 全域暫存路徑列表（所有 session，供翻頁使用；上限 200 筆）
static char s_allPaths[Limits::HISTORY_MAX > 200 ? 200 : Limits::HISTORY_MAX]
                      [Limits::SD_PATH_LEN];
static uint16_t s_allCount = 0;
/// @endcond


/**
 * @brief 進入 HISTORY 模式，載入所有 session 路徑並顯示第一頁
 */
void ModeHistory::onEnter() {
  _phase     = HistPhase::LIST;
  _pageStart = 0;
  s_allCount = 0;

  // 載入所有 session 路徑（最多 200 筆）
  SessionMgr::listSessions(s_allPaths,
                           sizeof(s_allPaths) / Limits::SD_PATH_LEN,
                           s_allCount);
  _totalCount = s_allCount;

  loadPage();
  drawListScreen();
  Serial.printf("[ModeHistory] onEnter: %u sessions\n", _totalCount);
}

/// @brief 每幀更新（純觸控 UI，無需更新）
void ModeHistory::update() {
  // 純觸控 UI，update 空閒
}

/// @brief 離開 HISTORY 模式
void ModeHistory::onExit() {
  Serial.println("[ModeHistory] onExit");
}

/**
 * @brief 按鍵事件處理（依 Phase 不同行為不同）
 *
 * LIST 階段：
 *  - BtnA：上一頁（有上一頁時）/ 返回 Home
 *  - BtnB：重新整理列表
 *  - BtnC：下一頁（有下一頁時）
 *
 * DETAIL 階段：
 *  - BtnA：返回 LIST
 *  - BtnC：進入 CONFIRM_DELETE
 *
 * CONFIRM_DELETE 階段：
 *  - BtnA：取消，返回 DETAIL
 *  - BtnC：確認刪除
 *
 * @param btn  按鍵索引（0=A / 1=B / 2=C）
 */
void ModeHistory::onButton(uint8_t btn) {
  if (_phase == HistPhase::LIST) {
    switch (btn) {
      case 0:  // BtnA — 上一頁 / Home
        if (_pageStart >= PAGE_SIZE) {
          _pageStart -= PAGE_SIZE;
          loadPage();
          drawListScreen();
        } else {
          GameFSM::requestSwitch(AppMode::HOME);
        }
        break;
      case 1:  // BtnB — 重新整理
        s_allCount = 0;
        SessionMgr::listSessions(s_allPaths,
                                 sizeof(s_allPaths) / Limits::SD_PATH_LEN,
                                 s_allCount);
        _totalCount = s_allCount;
        _pageStart  = 0;
        loadPage();
        drawListScreen();
        break;
      case 2:  // BtnC — 下一頁
        if (_pageStart + PAGE_SIZE < _totalCount) {
          _pageStart += PAGE_SIZE;
          loadPage();
          drawListScreen();
        }
        break;
      default: break;
    }
    return;
  }

  if (_phase == HistPhase::DETAIL) {
    switch (btn) {
      case 0:  // BtnA — 返回列表
        _phase = HistPhase::LIST;
        drawListScreen();
        break;
      case 2:  // BtnC — DELETE（進入確認畫面）
        _phase = HistPhase::CONFIRM_DELETE;
        drawConfirmDeleteScreen();
        break;
      default: break;
    }
    return;
  }

  if (_phase == HistPhase::CONFIRM_DELETE) {
    switch (btn) {
      case 0:  // BtnA — CANCEL，返回 DETAIL
        _phase = HistPhase::DETAIL;
        drawDetailScreen();
        break;
      case 2:  // BtnC — 確認刪除
        doDelete();
        break;
      default: break;
    }
    return;
  }
}

/**
 * @brief 觸控事件處理
 *
 * LIST：點擊列表項目 → 載入詳情並切換到 DETAIL
 * DETAIL：觸控任何地方 → 返回 LIST
 *
 * @param x  觸控 X 座標
 * @param y  觸控 Y 座標
 */
void ModeHistory::onTouch(int16_t x, int16_t y) {
  if (_phase == HistPhase::LIST) {
    constexpr int16_t ITEM_H  = 24;
    constexpr int16_t LIST_Y0 = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 2;
    if (y < LIST_Y0) return;

    int16_t relY = y - LIST_Y0;
    uint8_t localIdx = static_cast<uint8_t>(relY / ITEM_H);
    if (localIdx < _pageCount) {
      loadDetail(localIdx);
      drawDetailScreen();
    }
  } else if (_phase == HistPhase::DETAIL) {
    // 觸控任何地方返回列表
    _phase = HistPhase::LIST;
    drawListScreen();
  }
}


// ── Private helpers ───────────────────────────────────────────

/**
 * @brief 從 s_allPaths 載入目前頁的路徑到 _paths[]
 *
 * 根據 _pageStart 和 PAGE_SIZE 計算範圍，
 * 填入 _paths[] 並更新 _pageCount。
 */
void ModeHistory::loadPage() {
  _pageCount = 0;
  for (uint8_t i = 0; i < PAGE_SIZE; i++) {
    uint16_t globalIdx = _pageStart + i;
    if (globalIdx >= s_allCount) break;
    strlcpy(_paths[i], s_allPaths[globalIdx], Limits::SD_PATH_LEN);
    _pageCount++;
  }
}

/// @brief 繪製 Session 列表畫面（含翻頁按鈕）
void ModeHistory::drawListScreen() {
  UIScreen::clearGameArea();
  UIScreen::drawModeHeader("HISTORY", "SD CARD", 0xFF80);
  UIScreen::drawHistoryList(_paths, _pageCount, _totalCount,
                            _pageStart, SessionMgr::getSDFreeKB());
  bool hasPrev = (_pageStart > 0);
  bool hasNext = (_pageStart + PAGE_SIZE < _totalCount);
  UIScreen::drawBtnBar(hasPrev ? "PREV" : "HOME",
                       "REFRESH",
                       hasNext ? "NEXT" : "---");
}

/**
 * @brief 載入指定頁面索引的 session 詳情到 _detailRecord
 *
 * @param localIdx  目前頁面內的索引（0–_pageCount-1）
 */
void ModeHistory::loadDetail(uint8_t localIdx) {
  TT2_BOUNDS(localIdx, _pageCount, return);
  strlcpy(_detailPath, _paths[localIdx], sizeof(_detailPath));
  memset(&_detailRecord, 0, sizeof(_detailRecord));
  memset(&_drillDef, 0, sizeof(_drillDef));
  SessionMgr::loadSession(_detailPath, _detailRecord, &_drillDef);
  _phase = HistPhase::DETAIL;
  Serial.printf("[ModeHistory] Detail: %s hits=%u parMs=%u\n",
                _detailPath, _detailRecord.hit_count, _drillDef.parTimeMs);
}

/// @brief 繪製 Session 詳情畫面（shot rows + DRILL pass/fail）
void ModeHistory::drawDetailScreen() {
  UIScreen::clearGameArea();
  UIScreen::drawModeHeader("HISTORY", "DETAIL", 0xFF80);
  // DRILL mode 時傳入 drillDef，讓 UI 顯示 Pass/Fail highlight
  const DrillDef* dd = (_detailRecord.mode == AppMode::DRILL &&
                        _drillDef.parTimeMs > 0) ? &_drillDef : nullptr;
  UIScreen::drawHistoryDetail(_detailPath, _detailRecord, dd);
  UIScreen::drawBtnBar("BACK", nullptr, "DELETE");
}

/// @brief 繪製刪除確認畫面（顯示檔名和警告文字）
void ModeHistory::drawConfirmDeleteScreen() {
  UIScreen::clearGameArea();
  UIScreen::drawModeHeader("HISTORY", "DELETE?", 0xF800);

  // 確認提示
  constexpr int16_t CX = HW::LCD_W / 2;
  constexpr int16_t Y0 = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 20;

  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.drawString("Delete this session?", CX, Y0, GFXFF);

  // 檔名
  const char* fn = strrchr(_detailPath, '/');
  fn = fn ? fn + 1 : _detailPath;
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
  M5.Lcd.drawString(fn, CX, Y0 + 24, GFXFF);

  // 警告
  M5.Lcd.setTextColor(0xF800, TFT_BLACK);
  M5.Lcd.drawString("This cannot be undone!", CX, Y0 + 52, GFXFF);

  UIScreen::drawBtnBar("CANCEL", nullptr, "CONFIRM");
}

/**
 * @brief 執行刪除目前詳情頁的 session
 *
 * 呼叫 HalStorage::deleteSession() 刪除 SD 卡檔案，
 * 然後重新載入列表，若當前頁已無資料則往前一頁。
 */
void ModeHistory::doDelete() {
  bool ok = HalStorage::deleteSession(_detailPath);
  Serial.printf("[ModeHistory] Delete %s: %s\n", _detailPath, ok ? "OK" : "FAIL");

  // 刪除後重新載入列表，回到 LIST 頁
  s_allCount = 0;
  SessionMgr::listSessions(s_allPaths,
                           sizeof(s_allPaths) / Limits::SD_PATH_LEN,
                           s_allCount);
  _totalCount = s_allCount;
  // 若當前頁已無資料，往前一頁
  if (_pageStart >= _totalCount && _pageStart >= PAGE_SIZE)
    _pageStart -= PAGE_SIZE;
  else if (_pageStart >= _totalCount)
    _pageStart = 0;

  loadPage();
  _phase = HistPhase::LIST;
  drawListScreen();
}
