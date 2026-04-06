/**
 * @file    mode_history.h
 * @brief   TACTICAL TIMER 2 — History 模式（SD 卡 Session 瀏覽）宣告
 *
 * 功能：
 *  - 列出 SD 卡上所有 session（最新在前，最多顯示 20 筆）
 *  - 觸控選取單筆 session，顯示詳細分段資料
 *  - 顯示 SD 剩餘空間與 session 總數
 *
 * 畫面：LIST → DETAIL
 *
 * 按鍵：
 *  BtnA — 上一頁 / 返回列表 / 返回 Home
 *  BtnB — 重新整理列表
 *  BtnC — 下一頁 / 確認選取
 *
 * @version 2.0.0
 */

#pragma once
#include "game_fsm.h"


class ModeHistory : public ModeBase {
public:
  void    onEnter() override;
  void    update()  override;
  void    onExit()  override;
  AppMode getMode() const override { return AppMode::HISTORY; }
  void    onButton(uint8_t btn) override;
  void    onTouch(int16_t x, int16_t y) override;

private:
  enum class HistPhase : uint8_t { LIST, DETAIL, CONFIRM_DELETE };
  HistPhase _phase = HistPhase::LIST;

  // 列表分頁
  static constexpr uint8_t PAGE_SIZE = 6;
  char     _paths[PAGE_SIZE][Limits::SD_PATH_LEN];
  uint16_t _totalCount  = 0;
  uint16_t _pageStart   = 0;
  uint8_t  _pageCount   = 0;

  // 詳情
  GameRecord _detailRecord;
  DrillDef   _drillDef;       ///< DRILL mode 時有效，其他 mode 為 zero-init
  char       _detailPath[Limits::SD_PATH_LEN];

  void loadPage();
  void drawListScreen();
  void loadDetail(uint8_t localIdx);
  void drawDetailScreen();
  void drawConfirmDeleteScreen();
  void doDelete();
};
