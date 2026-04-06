/**
 * @file    hal_audio.h
 * @brief   TACTICAL TIMER 2 — I2S 音頻輸出 HAL 宣告
 *
 * 職責：
 *  - 封裝 ESP8266Audio 函式庫（AudioGeneratorMP3、AudioOutputI2S 等）
 *  - 管理 MP3 播放的完整生命週期（play / loop / done / release）
 *  - 與 IPC 協調 I2S 時分複用（播放前 pause MicTask，播完後 release）
 *
 * I2S 時分複用設計（關鍵）：
 *  AudioOutputI2S 的析構函式【不會】呼叫 i2s_driver_uninstall()，
 *  所以 onPlaybackDone() 必須手動呼叫 i2s_driver_uninstall(I2S_NUM_0)，
 *  才能讓 HalMic::init() 成功重新安裝 PDM driver。
 *
 * 呼叫順序（AudioTask）：
 *  1. play(path)        → IPC::requestMicPause() → 安裝 AudioOutputI2S
 *  2. loop()            → 每 loop() 一次，驅動解碼輸出
 *  3. onPlaybackDone()  → release() + i2s_driver_uninstall + IPC::releaseMicPause()
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include "ipc.h"
#include <AudioGeneratorMP3.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceID3.h>
#include <AudioOutputI2S.h>


namespace HalAudio {

  // ──────────────────────────────────────────────────────────
  // [A1] 播放控制
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 開始播放 SD 卡上的 MP3 檔案
   *
   * 流程：
   *  1. IPC::requestMicPause()  → 等待 MicTask 確認已暫停並釋放 I2S
   *  2. release()               → 確保上次的資源已清除
   *  3. 建立 AudioFileSourceSD / AudioFileSourceID3 / AudioOutputI2S
   *  4. AudioGeneratorMP3::begin()
   *
   * @param path  SD 卡路徑（e.g. "/mp3/7001.mp3"）
   * @return true  = 成功開始播放
   * @return false = 檔案找不到或 AudioGeneratorMP3::begin() 失敗
   */
  bool play(const char* path);

  /**
   * @brief 在 AudioTask 主迴圈中持續呼叫，驅動 MP3 解碼輸出
   *
   * @return true  = 播放中
   * @return false = 播放結束（呼叫方應接著呼叫 onPlaybackDone()）
   */
  bool loop();

  /**
   * @brief 播放完成（或中途 STOP）時的清理函式
   *
   * 流程：
   *  1. release()                       → 刪除所有 Audio 物件
   *  2. i2s_driver_uninstall(I2S_NUM_0) → 手動釋放 port（析構不會做）
   *  3. IPC::releaseMicPause()          → 通知 MicTask 可重新安裝 PDM
   */
  void onPlaybackDone();

  /**
   * @brief 強制停止播放（STOP 指令）
   *
   * 與 onPlaybackDone() 相同流程，但對外語意更清晰。
   * 呼叫後會送出 AudioDone{ok=false} 到 audioDoneQueue。
   */
  void stop();

  /**
   * @brief 釋放所有 Audio 動態物件（mp3/id3/out/file）
   *
   * 不呼叫 i2s_driver_uninstall；如需釋放 port 請用 onPlaybackDone()。
   */
  void release();


  // ──────────────────────────────────────────────────────────
  // [A2] 狀態查詢
  // ──────────────────────────────────────────────────────────

  /**
   * @return true = 目前正在播放 MP3
   */
  bool isPlaying();

} // namespace HalAudio
