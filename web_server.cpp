/**
 * @file    web_server.cpp
 * @brief   TACTICAL TIMER 2 — AsyncWebServer + SSE Web Dashboard 實作
 *
 * 嵌入完整 Web Dashboard HTML 於 PAGE_HTML PROGMEM 字串中，
 * 透過 AsyncWebServer 伺服 HTTP GET/POST 路由，
 * 以 SSE（Server-Sent Events）即時推播遊戲狀態到瀏覽器。
 *
 * 功能：
 *  - 模式切換、Start/Stop/Restart
 *  - Par Time、Random Delay、Drill 課題、Dry Fire BPM
 *  - Spy Listen/Stop/Clear、Preset 切換、Settings 雙向同步
 *  - History 瀏覽、詳情、刪除
 *  - SSE 事件：hit / gs / newgame / best / mode / settings / partime / drybeat
 *  - RWD：手機直式 / 橫式 / 平板 全適配
 *
 * 所有 HTTP handler 在 NetTask（Core 0）執行；
 * sendXxx() 可從任何 Task 安全呼叫（AsyncWebServer 內部加鎖）。
 *
 * @version 3.0
 */

#include "web_server.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Arduino.h>

// ============================================================
// PAGE_HTML — 全域 PROGMEM（namespace 外）
// ============================================================
static const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html lang="zh-TW"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="theme-color" content="#040d14">
<title>TT2 Remote</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
:root{
  --bg:#040d14;--bg2:#060f18;--bg3:#071420;--bg4:#0a1e2e;
  --bord:#1a5a72;--bord2:#0d3a52;
  --cyan:#00d9ff;--cyan2:#5ab0c8;--cyan3:#3a7a9a;
  --grn:#00ff88;--grn2:#00cc66;--grn3:#3a7a60;
  --yel:#ffc840;--red:#ff5050;--pur:#a0a0ff;
  --white:#e0f4ff;--dim:#2a4a5a;
  --tap:48px;
}
html,body{height:100%;background:var(--bg);color:var(--white);font-family:'Courier New',Courier,monospace;overflow:hidden}
 #app{display:flex;flex-direction:column;height:100dvh}

/* ── Header ── */
 #hd{display:flex;align-items:center;justify-content:space-between;padding:7px 14px;border-bottom:1px solid var(--bord);flex-shrink:0;background:var(--bg2)}
.logo{font-size:13px;font-weight:700;letter-spacing:4px;color:var(--cyan);font-family:sans-serif;line-height:1}
.logo em{opacity:.35;font-style:normal;font-weight:400}
 #hd-r{text-align:right;line-height:1}
 #clock{font-size:20px;color:var(--cyan);letter-spacing:2px}
 #date{font-size:9px;color:var(--cyan2);letter-spacing:2px;margin-top:1px}

/* ── Connection / state bar ── */
 #cb{display:flex;align-items:center;justify-content:space-between;padding:4px 14px;background:var(--bg2);border-bottom:1px solid var(--bord);flex-shrink:0}
.cbl{display:flex;align-items:center;gap:6px;font-size:10px;letter-spacing:2px;color:var(--cyan2)}
.dot{width:7px;height:7px;border-radius:50%;background:#1a3040;transition:background .3s;flex-shrink:0}
.dot.on{background:var(--grn)}
/* mode badge in cb */
 #mode-badge{font-size:9px;letter-spacing:2px;padding:2px 8px;border-radius:2px;text-transform:uppercase;background:var(--bg4);color:var(--cyan);border:1px solid var(--bord);margin-right:6px}
.gsb{font-size:10px;letter-spacing:2px;padding:2px 8px;border-radius:2px;text-transform:uppercase}
.gs-idle,.gs-stop{background:#0a1a24;color:var(--cyan2);border:1px solid var(--bord)}
.gs-areyouready,.gs-waiting,.gs-beeping{background:#1a1000;color:var(--yel);border:1px solid #3a2800}
.gs-going{background:#001a0a;color:var(--grn);border:1px solid #003a18}
.gs-showclear{background:#0a0a20;color:var(--pur);border:1px solid #2020a0}

/* ── Tabs ── */
 #tabs{display:flex;background:var(--bg);border-bottom:2px solid var(--bord);flex-shrink:0}
.tab{flex:1;padding:0 4px;height:44px;text-align:center;font-size:10px;letter-spacing:2px;color:var(--cyan2);cursor:pointer;text-transform:uppercase;border-bottom:2px solid transparent;margin-bottom:-2px;transition:color .2s,border-color .2s;user-select:none;display:flex;align-items:center;justify-content:center}
.tab.active{color:var(--cyan);border-bottom:2px solid var(--cyan)}

/* ── Panels ── */
 #panels{flex:1;overflow:hidden;position:relative;min-height:0}
.panel{position:absolute;inset:0;overflow-y:auto;overflow-x:hidden;display:none;flex-direction:column}
.panel.active{display:flex}

/* ── Card ── */
.card{background:var(--bg3);border:1px solid var(--bord);border-radius:6px;padding:10px 12px;margin:0 10px 8px}
.ctitle{font-size:9px;letter-spacing:3px;color:var(--cyan2);text-transform:uppercase;margin-bottom:8px;padding-bottom:6px;border-bottom:1px solid var(--bord2)}

/* ══════════════════════════════
   LIVE
══════════════════════════════ */
 #panel-live{padding-top:10px}

/* Stats bar — 2 rows on mobile: shots full-width, then total+avg */
 #stats{display:grid;grid-template-columns:1fr 1fr;gap:6px;padding:0 10px;margin-bottom:10px}
 #sc-shots{grid-column:span 2}  /* shots takes full width on mobile */
.sc{background:var(--bg3);border:1px solid var(--bord);border-left:3px solid var(--cyan);border-radius:4px;padding:10px 12px}
.sl{font-size:8px;letter-spacing:3px;color:var(--cyan2);text-transform:uppercase;margin-bottom:4px}
.sv{font-size:28px;color:#fff;letter-spacing:1px;line-height:1}
.sv small{font-size:11px;color:var(--cyan);margin-left:2px}
/* shots card horizontal layout */
 #sc-shots{display:flex;align-items:center;justify-content:space-between;padding:8px 12px}
 #sc-shots .sl{margin-bottom:0;margin-right:8px}
 #sc-shots .sv{font-size:32px}
/* mode tag inside shots card */
.live-mode{font-size:9px;letter-spacing:3px;color:var(--cyan);text-transform:uppercase;padding:3px 8px;border:1px solid var(--bord);border-radius:2px;background:var(--bg4)}
.brow{display:flex;align-items:center;gap:3px;margin-top:4px}
.blbl{font-size:8px;letter-spacing:2px;color:var(--grn3);text-transform:uppercase}
.bval{font-size:12px;color:var(--grn2)}
.bval small{font-size:8px;color:var(--grn3);margin-left:1px}

/* Shot log grid */
.sgrid{display:grid;grid-template-columns:1fr 1fr;gap:2px;background:var(--bord);border-radius:4px;overflow:hidden;margin:0 10px 10px}
.scol{background:var(--bg);display:flex;flex-direction:column}
.shdr{display:grid;grid-template-columns:22px 36px 1fr 56px;padding:5px 8px;background:var(--bg3)}
.shc{font-size:8px;letter-spacing:1px;color:var(--cyan3);text-transform:uppercase}
.srows{overflow-y:auto;max-height:260px}
.srow{display:grid;grid-template-columns:22px 36px 1fr 56px;align-items:center;padding:9px 8px;border-bottom:1px solid var(--bg3)}
/* 手機隱藏 SRC 欄 */
@media(max-width:480px){
  .shdr{grid-template-columns:24px 1fr 60px}
  .srow{grid-template-columns:24px 1fr 60px}
  .shc-src,.srow-src{display:none}
}
.srow.new{animation:fi .6s ease-out}
@keyframes fi{from{background:#002030}to{background:var(--bg)}}
/* par-fail highlight */
.srow.par-fail .tv{color:var(--red)}
.srow.par-fail .sp{color:var(--red)}
.rn{font-size:11px;color:var(--cyan3)}
.bdg{font-size:8px;padding:2px 4px;border-radius:2px;white-space:nowrap}
.bdg.esp{background:#0a2030;color:#40b8e0;border:1px solid var(--bord)}
.bdg.mic{background:#1a1200;color:var(--yel);border:1px solid #3a2800}
.tv{font-size:19px;color:#fff;letter-spacing:1px;line-height:1;transition:color .3s}
.tv small{font-size:9px;color:var(--cyan2);margin-left:1px}
.sp{font-size:13px;text-align:right;letter-spacing:1px;transition:color .3s}
/* split colour: fast=green  ok=cyan  slow=yellow  very slow=red */
.sp.f{color:var(--grn)}.sp.ok{color:var(--cyan2)}.sp.w{color:var(--yel)}.sp.s{color:var(--red)}
.sp small{font-size:8px}
.empty{padding:18px;text-align:center;font-size:10px;letter-spacing:3px;color:var(--dim)}

/* ══════════════════════════════
   CONTROL
══════════════════════════════ */
 #panel-ctrl{padding-top:0}
.mode-bar{background:var(--bg2);border-bottom:1px solid var(--bord);padding:8px 10px;flex-shrink:0}
.mbars-title{font-size:8px;letter-spacing:3px;color:var(--cyan2);text-transform:uppercase;margin-bottom:6px}
.mode-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:5px}
.mbtn{background:var(--bg3);border:1px solid var(--bord);border-radius:6px;padding:12px 4px;text-align:center;cursor:pointer;user-select:none;transition:background .15s,border-color .15s;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:5px;min-height:76px}
.mbtn:active{transform:scale(.95)}
.mbtn.am{border-color:var(--cyan);background:var(--bg4)}
.micon{font-size:22px;line-height:1}
.mlbl{font-size:10px;letter-spacing:2px;color:var(--cyan2);text-transform:uppercase;font-weight:700}
.mbtn.am .mlbl{color:var(--cyan)}
/* switching animation */
.mbtn.switching{border-color:var(--yel);animation:pulse .4s ease-out}
@keyframes pulse{0%{opacity:.5}100%{opacity:1}}

.ctrl-content{flex:1;overflow-y:auto;padding-top:6px}

/* Game state box */
.gsbox{text-align:center;padding:6px 0 2px}
.gs-lbl{font-size:9px;letter-spacing:4px;color:var(--cyan2);text-transform:uppercase;margin-bottom:4px}
.gs-val{font-size:26px;letter-spacing:3px;font-weight:700;text-transform:uppercase;transition:color .3s}
.gs-val.idle,.gs-val.stop{color:var(--cyan2)}
.gs-val.areyouready,.gs-val.waiting,.gs-val.beeping{color:var(--yel)}
.gs-val.going{color:var(--grn)}
.gs-val.showclear{color:var(--pur)}

/* Action buttons */
.abtns{display:grid;grid-template-columns:1fr 1fr;gap:8px;padding:0 10px 8px}
.abtns.three{grid-template-columns:1fr 1fr 1fr}
.abtn{padding:11px 6px;border-radius:5px;font-size:12px;letter-spacing:3px;text-transform:uppercase;cursor:pointer;font-family:'Courier New',monospace;border:1px solid;outline:none;min-height:44px;transition:transform .1s,opacity .15s}
.abtn:active{transform:scale(.95)}
.abtn.stop{background:#1a0000;color:var(--red);border-color:#6a0000}
.abtn.stop:not(:disabled):active{background:#2a0000}
.abtn.start{background:#001a08;color:var(--grn);border-color:#006a20}
.abtn.start:not(:disabled):active{background:#002a10}
.abtn.restart{background:#0a0a00;color:var(--yel);border-color:#4a3800}
.abtn.listen{background:#001a24;color:var(--cyan);border-color:var(--bord)}
.abtn:disabled{opacity:.2;cursor:not-allowed}

/* Control rows */
.crow{display:flex;align-items:center;justify-content:space-between;padding:7px 0;border-bottom:1px solid var(--bord2);gap:8px}
.crow:last-child{border-bottom:none}
.clbl{font-size:14px;color:var(--white);letter-spacing:1px}
.csub{font-size:10px;color:var(--cyan2);margin-top:2px}

/* Toggle */
.tog{display:flex;gap:2px;background:var(--bg);border:1px solid var(--bord);border-radius:4px;padding:2px;flex-shrink:0}
.topt{padding:8px 12px;border-radius:3px;font-size:12px;letter-spacing:2px;cursor:pointer;text-transform:uppercase;font-family:'Courier New',monospace;border:none;outline:none;background:transparent;color:var(--cyan2);min-height:40px;transition:background .15s,color .15s}
.topt.on{background:var(--bg4);color:var(--cyan)}

/* Stepper — enlarged tap targets */
.stepper{display:flex;align-items:center;gap:4px;flex-shrink:0}
.stpbtn{width:44px;height:44px;border-radius:5px;background:var(--bg3);border:1px solid var(--bord);color:var(--cyan);font-size:22px;cursor:pointer;display:flex;align-items:center;justify-content:center;flex-shrink:0;transition:background .1s}
.stpbtn:active{background:var(--bg4)}
.stpval{font-size:16px;color:#fff;min-width:56px;text-align:center;line-height:1.2}
.stpunit{font-size:12px;color:var(--cyan2);text-align:center;margin-top:1px}

/* Par time display */
.par-display{font-size:22px;color:var(--cyan);font-weight:700;letter-spacing:2px;min-width:70px;text-align:center}

/* BPM */
.bpm-wrap{text-align:center;padding:8px 0 2px}
.bpm-big{font-size:60px;color:#fff;letter-spacing:2px;line-height:1}
.bpm-unit{font-size:11px;color:var(--cyan2);letter-spacing:3px;margin-top:2px}
.bpm-ms{font-size:12px;color:var(--cyan);margin-top:4px;letter-spacing:1px}
.slider-wrap{padding:0 16px;margin:10px 0 4px}
.hint-row{font-size:9px;letter-spacing:1px;color:var(--dim);display:flex;justify-content:space-between;margin-top:4px}
input[type=range]{flex:1;accent-color:var(--cyan);cursor:pointer;height:6px;border-radius:3px}

/* Spy stats */
.spy-stats{display:grid;grid-template-columns:1fr 1fr;gap:8px;padding:0 10px;margin-bottom:10px}
.spy-s{background:var(--bg3);border:1px solid var(--bord);border-radius:4px;padding:12px;text-align:center}
.spy-sl{font-size:8px;letter-spacing:2px;color:var(--cyan2);margin-bottom:4px}
.spy-sv{font-size:28px;color:#fff}

/* ══════════════════════════════
   SETTINGS
══════════════════════════════ */
 #panel-sett{padding-top:10px}
.srow{display:flex;align-items:center;justify-content:space-between;padding:12px 0;border-bottom:1px solid var(--bord2);gap:10px}
.srow:last-child{border-bottom:none}
.slbl{font-size:12px;color:var(--white);letter-spacing:1px}
.ssub{font-size:9px;color:var(--cyan2);margin-top:2px;letter-spacing:1px}
.thr-block{width:100%;margin-top:6px}
.thr-row{display:flex;align-items:center;gap:10px;margin-bottom:4px}
.thr-val{font-size:28px;color:#fff;letter-spacing:2px;min-width:68px;text-align:right}
.thr-unit{font-size:10px;color:var(--cyan2)}
.sbtn{width:100%;padding:14px;background:#001a24;color:var(--cyan);border:1px solid var(--cyan);border-radius:4px;font-size:12px;letter-spacing:4px;text-transform:uppercase;cursor:pointer;font-family:'Courier New',monospace;min-height:var(--tap);transition:background .2s,color .2s,border-color .2s}
.sbtn.dirty{background:#001a24;border-color:var(--yel);color:var(--yel)}  /* unsaved changes indicator */
.sbtn.saved{background:#001a08;color:var(--grn);border-color:var(--grn)}
/* preset list */
.plist{display:flex;flex-direction:column;gap:6px}
.pitem{display:flex;align-items:center;justify-content:space-between;padding:10px 12px;background:var(--bg);border:1px solid var(--bord);border-radius:4px;cursor:pointer;min-height:var(--tap);transition:background .15s,border-color .15s}
.pitem:active{background:var(--bg4)}
.pitem.ap{border-color:var(--cyan);background:var(--bg4)}
.pname{font-size:14px;color:var(--white);font-weight:700}
.pdetail{font-size:12px;color:var(--cyan2);margin-top:3px;letter-spacing:1px}
.pchk{width:20px;height:20px;border-radius:50%;background:transparent;border:1px solid var(--bord);display:flex;align-items:center;justify-content:center;flex-shrink:0;transition:background .15s,border-color .15s}
.pitem.ap .pchk{background:var(--cyan);border-color:var(--cyan);color:#000;font-size:11px}

/* ══════════════════════════════
   HISTORY
══════════════════════════════ */
 #panel-hist{padding-top:10px}
/* filter bar */
.hfilter{display:flex;gap:6px;padding:0 10px;margin-bottom:10px;overflow-x:auto}
.hftag{padding:5px 12px;border-radius:12px;font-size:9px;letter-spacing:2px;text-transform:uppercase;border:1px solid var(--bord);color:var(--cyan2);cursor:pointer;white-space:nowrap;transition:background .15s,color .15s}
.hftag.on{background:var(--bg4);border-color:var(--cyan);color:var(--cyan)}
.hitem-wrap{display:flex;align-items:stretch;margin:0 10px 8px}
.hitem{flex:1;background:var(--bg3);border:1px solid var(--bord);border-radius:5px 0 0 5px;padding:11px 12px;cursor:pointer;display:flex;align-items:center;justify-content:space-between;transition:background .15s}
.hitem:active{background:var(--bg4)}
.hl{flex:1;min-width:0}
.hdate{font-size:10px;color:var(--cyan2);letter-spacing:1px;margin-bottom:3px}
.hmode-tag{display:inline-block;font-size:8px;letter-spacing:2px;color:var(--cyan);text-transform:uppercase;border:1px solid var(--bord2);border-radius:2px;padding:1px 5px;margin-bottom:3px}
.hmode-tag.drill{color:var(--yel);border-color:#3a2800}
.hmode-tag.ro{color:#a0c0ff;border-color:#202060}
.hmode-tag.spy{color:var(--grn2);border-color:#1a3a20}
.hr{text-align:right;flex-shrink:0;padding-left:8px}
.hshots{font-size:22px;color:#fff;line-height:1}
.hshots small{font-size:9px;color:var(--cyan2);margin-left:2px}
.htime{font-size:11px;color:var(--cyan);margin-top:2px;font-weight:700}
.havg{font-size:9px;color:var(--cyan2);margin-top:1px}
/* pass/fail badge on list item */
.hpf{font-size:8px;letter-spacing:2px;padding:2px 6px;border-radius:2px;margin-top:3px;display:inline-block}
.hpf.p{background:#001a08;color:var(--grn);border:1px solid #006a20}
.hpf.f{background:#1a0000;color:var(--red);border:1px solid #6a0000}
.hdel{padding:0 12px;background:transparent;border:1px solid #2a0808;border-left:none;color:#703030;border-radius:0 5px 5px 0;font-size:9px;letter-spacing:2px;cursor:pointer;font-family:'Courier New',monospace;min-height:var(--tap);transition:background .15s,color .15s}
.hdel:active{background:#1a0000;color:var(--red)}
.hempty{text-align:center;padding:40px 20px;font-size:10px;letter-spacing:3px;color:var(--dim)}

/* Detail overlay */
 #det{position:fixed;inset:0;background:var(--bg);z-index:100;display:none;flex-direction:column}
 #det.show{display:flex}
.det-hd{display:flex;align-items:center;gap:10px;padding:10px 14px;border-bottom:1px solid var(--bord);flex-shrink:0;background:var(--bg2)}
.back{padding:8px 14px;background:var(--bg3);border:1px solid var(--bord);border-radius:4px;color:var(--cyan);font-size:11px;letter-spacing:2px;cursor:pointer;font-family:'Courier New',monospace;min-height:var(--tap);display:flex;align-items:center}
.det-title{font-size:11px;letter-spacing:2px;color:var(--cyan);flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.det-del{padding:8px 12px;background:transparent;border:1px solid #3a1010;color:#804040;border-radius:4px;font-size:9px;letter-spacing:2px;cursor:pointer;font-family:'Courier New',monospace;min-height:var(--tap);display:flex;align-items:center}
.det-body{padding:10px;flex:1;overflow-y:auto}
/* detail stats */
.dstats{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-bottom:10px}
.dstat{background:var(--bg3);border:1px solid var(--bord);border-radius:4px;padding:8px 10px;text-align:center}
.dslbl{font-size:8px;letter-spacing:2px;color:var(--cyan2);margin-bottom:3px;text-transform:uppercase}
.dsval{font-size:20px;color:#fff;line-height:1}
.dsval small{font-size:9px;color:var(--cyan2)}
/* drill result */
.drill-badge{text-align:center;padding:6px 10px;margin-bottom:8px;border-radius:5px}
.dpas{background:#001a08;border:1px solid #006a20;color:var(--grn)}
.dfai{background:#1a0000;border:1px solid #6a0000;color:var(--red)}
.drill-lbl{font-size:16px;font-weight:700;letter-spacing:3px}
.drill-sub{font-size:9px;letter-spacing:1px;margin-top:3px;opacity:.75}
/* split bar chart */
.split-chart{margin-bottom:10px}
.split-chart-title{font-size:9px;letter-spacing:3px;color:var(--cyan2);text-transform:uppercase;margin-bottom:6px}
.bar-row{display:flex;align-items:center;gap:6px;margin-bottom:4px}
.bar-label{font-size:10px;color:var(--cyan3);width:20px;text-align:right;flex-shrink:0}
.bar-track{flex:1;background:var(--bg3);border-radius:2px;overflow:hidden;height:18px;position:relative}
.bar-fill{height:100%;border-radius:2px;transition:width .4s ease-out;display:flex;align-items:center;padding-left:6px;min-width:2px}
.bar-val{font-size:9px;color:#fff;white-space:nowrap;position:absolute;left:6px;top:50%;transform:translateY(-50%)}
/* par line on bar */
.par-line{position:absolute;top:0;bottom:0;width:2px;background:var(--yel);opacity:.7}

/* ══════════════════════════════
   SYSTEM
══════════════════════════════ */
 #panel-sys{padding-top:12px}
.sys-card{background:var(--bg3);border:1px solid var(--bord);border-radius:6px;margin:0 10px 10px;overflow:hidden}
.sys-item{display:flex;align-items:center;justify-content:space-between;padding:14px 16px;border-bottom:1px solid var(--bord2);cursor:pointer;transition:background .15s;min-height:var(--tap)}
.sys-item:last-child{border-bottom:none}
.sys-item:active{background:var(--bg4)}
.sys-lbl{font-size:13px;color:var(--white);letter-spacing:1px}
.sys-sub{font-size:10px;color:var(--cyan2);margin-top:2px}
.sys-icon{font-size:20px;flex-shrink:0;margin-right:14px}
.sys-chevron{font-size:14px;color:var(--cyan2);flex-shrink:0}
/* theme selector */
.theme-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;padding:10px 12px}
.theme-btn{border-radius:6px;padding:10px 6px;text-align:center;cursor:pointer;border:2px solid transparent;transition:border-color .15s;min-height:56px;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px}
.theme-btn.active-theme{border-color:var(--cyan)}
.theme-swatch{width:28px;height:18px;border-radius:3px;margin:0 auto}
.theme-name{font-size:9px;letter-spacing:2px;color:var(--cyan2);text-transform:uppercase}
/* confirm dialog */
.sys-confirm{display:none;position:fixed;inset:0;background:rgba(0,0,0,.7);z-index:200;align-items:center;justify-content:center}
.sys-confirm.show{display:flex}
.sys-dlg{background:var(--bg3);border:1px solid var(--bord);border-radius:8px;padding:24px 20px;max-width:300px;width:90%;text-align:center}
.sys-dlg-title{font-size:16px;color:var(--white);letter-spacing:2px;margin-bottom:8px}
.sys-dlg-sub{font-size:11px;color:var(--cyan2);margin-bottom:20px;line-height:1.5}
.sys-dlg-btns{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.sys-dlg-cancel{padding:12px;background:var(--bg);border:1px solid var(--bord);border-radius:4px;color:var(--cyan2);font-size:12px;letter-spacing:2px;cursor:pointer;font-family:'Courier New',monospace}
.sys-dlg-ok{padding:12px;border:1px solid;border-radius:4px;font-size:12px;letter-spacing:2px;cursor:pointer;font-family:'Courier New',monospace}
.sys-dlg-ok.danger{background:#1a0000;color:var(--red);border-color:#6a0000}
@media(min-width:480px){
   #sc-shots{grid-column:auto}  /* 3-column on wider screens */
   #stats{grid-template-columns:repeat(3,1fr)}
}
@media(min-width:560px){
  .abtns.three{grid-template-columns:1fr 1fr 1fr}
  .srows{max-height:320px}
  .dstats{grid-template-columns:repeat(4,1fr)}
}
@media(min-width:860px){
   #app{max-width:860px;margin:0 auto}
  .sgrid .srows{max-height:400px}
}
@media(orientation:landscape)and(max-height:500px){
  .bpm-big{font-size:40px}
  .gsbox{padding:4px 0}
  .gs-val{font-size:22px}
  .abtns{padding-bottom:6px}
}
</style></head><body>
<div id="app">
  <!-- Header -->
  <div id="hd">
    <div class="logo">TT2<em> // </em>REMOTE</div>
    <div id="hd-r"><div id="clock">--:--:--</div><div id="date">---- . -- . --</div></div>
  </div>
  <!-- Connection bar -->
  <div id="cb">
    <div class="cbl"><span class="dot" id="dot"></span><span id="clabel">WAITING...</span></div>
    <div style="display:flex;align-items:center;gap:6px">
      <span id="mode-badge">--</span>
      <span class="gsb gs-idle" id="gsb">STANDBY</span>
    </div>
  </div>
  <!-- Tabs -->
  <div id="tabs">
    <div class="tab" onclick="sw('live')">LIVE</div>
    <div class="tab active" onclick="sw('ctrl')">CONTROL</div>
    <div class="tab" onclick="sw('sett')">SETTINGS</div>
    <div class="tab" onclick="sw('sys')">SYSTEM</div>
  </div>
  <div id="panels">

    <!-- ══ LIVE ══ -->
    <div class="panel" id="panel-live">
      <!-- Drill result banner (hidden until drill ends) -->
      <div id="drill-result-banner" style="display:none;margin:6px 10px 0">
        <div class="drill-badge" id="drill-result-inner">
          <div class="drill-lbl" id="drill-result-lbl">PASSED ✓</div>
          <div class="drill-sub" id="drill-result-sub"></div>
        </div>
      </div>
      <div id="stats">
        <!-- Shots (spans full width on mobile) -->
        <div class="sc" id="sc-shots">
          <div style="display:flex;align-items:center;justify-content:space-between">
            <div>
              <div class="sl">Shots</div>
              <div class="sv" id="s-shots">0<small id="s-shots-max" style="font-size:12px;color:var(--cyan2);margin-left:3px">/ 10</small></div>
            </div>
            <span class="live-mode" id="live-mode-tag">FREE</span>
          </div>
        </div>
        <!-- Draw (RO mode only) -->
        <div class="sc" id="sc-draw" style="display:none;grid-column:span 2;border-left-color:var(--yel)">
          <div class="sl">Draw Time</div>
          <div class="sv" id="s-draw" style="color:var(--yel)">--<small>s</small></div>
        </div>
        <!-- Total -->
        <div class="sc">
          <div class="sl" id="lbl-total">Total</div>
          <div class="sv" id="s-total">--<small>s</small></div>
          <div class="brow"><span class="blbl">BEST</span>&nbsp;<span class="bval" id="bt">--<small>s</small></span></div>
        </div>
        <!-- Avg Split -->
        <div class="sc">
          <div class="sl" id="lbl-avg">Avg Split</div>
          <div class="sv" id="s-avg">--<small>s</small></div>
          <div class="brow"><span class="blbl">BEST</span>&nbsp;<span class="bval" id="ba">--<small>s</small></span></div>
        </div>
      </div>
      <!-- Shot log -->
      <div class="sgrid">
        <div class="scol">
          <div class="shdr"><div class="shc">#</div><div class="shc shc-src">SRC</div><div class="shc">TIME</div><div class="shc" style="text-align:right">SPL</div></div>
          <div class="srows" id="L"><div class="empty">--- WAITING ---</div></div>
        </div>
        <div class="scol">
          <div class="shdr"><div class="shc">#</div><div class="shc shc-src">SRC</div><div class="shc">TIME</div><div class="shc" style="text-align:right">SPL</div></div>
          <div class="srows" id="R"><div class="empty">--- WAITING ---</div></div>
        </div>
      </div>
    </div>

    <!-- ══ CONTROL ══ -->
    <div class="panel active" id="panel-ctrl">
      <!-- Mode selector -->
      <div class="mode-bar">
        <div class="mbars-title">SELECT MODE</div>
        <div class="mode-grid" id="mgrid">
          <div class="mbtn" onclick="switchMode(2)" data-m="2"><div class="micon">🎯</div><div class="mlbl">FREE</div></div>
          <div class="mbtn" onclick="switchMode(3)" data-m="3"><div class="micon">📋</div><div class="mlbl">DRILL</div></div>
          <div class="mbtn" onclick="switchMode(4)" data-m="4"><div class="micon">🔔</div><div class="mlbl">DRY FIRE</div></div>
          <div class="mbtn" onclick="switchMode(5)" data-m="5"><div class="micon">👁</div><div class="mlbl">SPY</div></div>
          <div class="mbtn" onclick="switchMode(6)" data-m="6"><div class="micon">🎖</div><div class="mlbl">RO</div></div>
          <div class="mbtn" onclick="switchMode(7)" data-m="7"><div class="micon">📜</div><div class="mlbl">HISTORY</div></div>
        </div>
      </div>
      <div class="ctrl-content">

        <!-- FREE -->
        <div id="c-free" style="display:none">
          <div class="gsbox"><div class="gs-lbl">GAME STATE</div><div class="gs-val idle" id="gs-free">STANDBY</div></div>
          <div class="abtns">
            <button class="abtn stop"  id="bs-f"  onclick="sendCmd('stop')"  disabled>STOP</button>
            <button class="abtn start" id="bst-f" onclick="startAndJump()">START</button>
          </div>
          <div class="card">
            <div class="ctitle">FREE SETTINGS</div>
            <div class="crow">
              <div><div class="clbl">Random Delay</div><div class="csub">Randomize start beep timing</div></div>
              <div class="tog">
                <button class="topt on" id="rnd-off" onclick="setRand(false)">OFF</button>
                <button class="topt"    id="rnd-on"  onclick="setRand(true)">ON</button>
              </div>
            </div>
            <div class="crow">
              <div style="flex:1"><div class="clbl">Par Time</div><div class="csub" id="par-sub">Disabled</div></div>
              <div class="stepper">
                <button class="stpbtn" onclick="adjPar(-250)">&#8722;</button>
                <div><div class="par-display" id="par-val">OFF</div><div class="stpunit">ms</div></div>
                <button class="stpbtn" onclick="adjPar(250)">+</button>
              </div>
            </div>
          </div>
        </div>

        <!-- DRILL -->
        <div id="c-drill" style="display:none">
          <div class="gsbox"><div class="gs-lbl">GAME STATE</div><div class="gs-val idle" id="gs-drill">STANDBY</div></div>
          <div class="abtns">
            <button class="abtn stop"  id="bs-d"  onclick="sendCmd('stop')"  disabled>STOP</button>
            <button class="abtn start" id="bst-d" onclick="startAndJump()">START</button>
          </div>
          <div class="card">
            <div class="ctitle">DRILL SETUP</div>
            <div class="crow">
              <div style="flex:1"><div class="clbl">Shots</div><div class="csub">Target count (1–10)</div></div>
              <div class="stepper">
                <button class="stpbtn" onclick="adjD('s',-1)">&#8722;</button>
                <div><div class="stpval" id="d-shots">6</div></div>
                <button class="stpbtn" onclick="adjD('s',1)">+</button>
              </div>
            </div>
            <div class="crow">
              <div style="flex:1"><div class="clbl">Par Time</div><div class="csub" id="d-par-sub">No limit</div></div>
              <div class="stepper">
                <button class="stpbtn" onclick="adjD('p',-100)">&#8722;</button>
                <div><div class="par-display" id="d-par">OFF</div><div class="stpunit">ms</div></div>
                <button class="stpbtn" onclick="adjD('p',100)">+</button>
              </div>
            </div>
            <div class="crow">
              <div style="flex:1"><div class="clbl">Pass %</div><div class="csub">Min shots within par</div></div>
              <div class="stepper">
                <button class="stpbtn" onclick="adjD('q',-10)">&#8722;</button>
                <div><div class="stpval" id="d-pass">80</div><div class="stpunit">%</div></div>
                <button class="stpbtn" onclick="adjD('q',10)">+</button>
              </div>
            </div>
          </div>
        </div>

        <!-- DRY FIRE -->
        <div id="c-dry" style="display:none">
          <div class="card">
            <div class="ctitle">DRY FIRE METRONOME</div>
            <!-- 主顯示：ms（與 Core2 一致）-->
            <div class="bpm-wrap">
              <div class="bpm-big" id="bpm-ms-big">2000</div>
              <div class="bpm-unit">ms / beat</div>
              <div class="bpm-ms" id="bpm-ref">30 BPM</div>
            </div>
            <!-- Stepper：步進 200ms，與 Core2 BtnB/C 一致 -->
            <div style="display:flex;align-items:center;justify-content:center;gap:16px;margin:10px 0 4px">
              <button class="stpbtn" style="width:52px;height:52px;font-size:26px" onclick="adjDry(-200)">&#8722;</button>
              <div style="text-align:center">
                <div style="font-size:10px;letter-spacing:2px;color:var(--cyan2)">SLOWER &nbsp; FASTER</div>
              </div>
              <button class="stpbtn" style="width:52px;height:52px;font-size:26px" onclick="adjDry(200)">+</button>
            </div>
            <div class="hint-row" style="margin:0 4px 8px"><span>500ms max slow</span><span>5000ms max fast</span></div>
            <div class="abtns" style="margin:4px 0 0">
              <button class="abtn stop"  id="dry-stop"  onclick="sendDry('stop')"  disabled>STOP</button>
              <button class="abtn start" id="dry-start" onclick="sendDry('start')">START</button>
            </div>
          </div>
        </div>

        <!-- SPY -->
        <div id="c-spy" style="display:none">
          <div class="gsbox"><div class="gs-lbl">SPY STATE</div><div class="gs-val idle" id="gs-spy">STANDBY</div></div>
          <div class="abtns three">
            <button class="abtn stop"    id="spy-stop"   onclick="sendSpy('stop')"   disabled>STOP</button>
            <button class="abtn listen"  id="spy-listen" onclick="sendSpy('listen')">LISTEN</button>
            <button class="abtn restart" id="spy-clear"  onclick="sendSpy('clear')">CLEAR</button>
          </div>
          <div class="card">
            <div class="ctitle">SPY STATISTICS</div>
            <div class="crow">
              <div><div class="clbl">Shots Detected</div><div class="csub">Total mic triggers</div></div>
              <div class="stpval" id="spy-n" style="font-size:22px;color:#fff;min-width:48px;text-align:right">0</div>
            </div>
            <div class="crow">
              <div><div class="clbl">Total Spread</div><div class="csub">First to last shot</div></div>
              <div class="stpval" id="spy-t" style="font-size:18px;color:var(--cyan);min-width:64px;text-align:right">--</div>
            </div>
          </div>
        </div>

        <!-- RO -->
        <div id="c-ro" style="display:none">
          <div class="gsbox"><div class="gs-lbl">GAME STATE</div><div class="gs-val idle" id="gs-ro">STANDBY</div></div>
          <div class="abtns">
            <button class="abtn stop"  id="bs-ro"  onclick="sendCmd('stop')"  disabled>STOP</button>
            <button class="abtn start" id="bst-ro" onclick="startAndJump()">START</button>
          </div>
          <div class="card">
            <div class="ctitle">RO SETTINGS</div>
            <div class="crow">
              <div><div class="clbl">Random Delay</div><div class="csub">Randomize fire beep</div></div>
              <div class="tog">
                <button class="topt on" id="ro-rnd-off" onclick="setRand(false)">OFF</button>
                <button class="topt"    id="ro-rnd-on"  onclick="setRand(true)">ON</button>
              </div>
            </div>
          </div>
        </div>

      </div><!-- /ctrl-content -->
    </div><!-- /panel-ctrl -->

    <!-- ══ SETTINGS ══ -->
    <div class="panel" id="panel-sett">
      <div class="card" style="margin-top:0;border-radius:0;border-left:none;border-right:none;border-top:none">
        <div class="ctitle">HIT SOURCE</div>
        <div class="srow" style="border:none">
          <div><div class="slbl">Detection Mode</div><div class="ssub">How hits are registered</div></div>
          <div class="tog">
            <button class="topt"    id="opt-esp"  onclick="setSrc(0)">E-NOW</button>
            <button class="topt on" id="opt-mic"  onclick="setSrc(1)">MIC</button>
            <button class="topt"    id="opt-both" onclick="setSrc(2)">BOTH</button>
          </div>
        </div>
      </div>
      <div class="card" id="mic-card">
        <div class="ctitle">MIC THRESHOLD</div>
        <div class="thr-block">
          <div class="thr-row">
            <input type="range" id="thr" min="500" max="8000" step="200" value="2000" oninput="updThr(this.value)">
            <div><span class="thr-val" id="thr-val">2000</span><span class="thr-unit"> RMS</span></div>
          </div>
          <div class="hint-row"><span>500 sensitive</span><span>8000 robust</span></div>
        </div>
      </div>
      <div class="card">
        <div class="ctitle">PRESETS</div>
        <div class="plist" id="plist"><div class="empty">Loading...</div></div>
      </div>
    </div>

    <!-- ══ HISTORY (overlay only, accessed via CONTROL card) ══ -->
    <div class="panel" id="panel-hist" style="display:none!important"></div>

    <!-- ══ SYSTEM ══ -->
    <div class="panel" id="panel-sys">
      <!-- Theme -->
      <div class="sys-card" style="margin-top:0;border-radius:0;border-left:none;border-right:none;border-top:none">
        <div class="ctitle" style="padding:10px 14px 8px;margin:0;border-bottom:1px solid var(--bord2)">DISPLAY THEME</div>
        <div class="theme-grid" id="theme-grid">
          <div class="theme-btn active-theme" onclick="setTheme('dark')" id="th-dark">
            <div class="theme-swatch" style="background:#040d14;border:1px solid #1a5a72"></div>
            <div class="theme-name">DARK</div>
          </div>
          <div class="theme-btn" onclick="setTheme('navy')" id="th-navy">
            <div class="theme-swatch" style="background:#0a0f1e;border:1px solid #2a4a8a"></div>
            <div class="theme-name">NAVY</div>
          </div>
          <div class="theme-btn" onclick="setTheme('light')" id="th-light">
            <div class="theme-swatch" style="background:#f0f4f8;border:1px solid #8ab0c8"></div>
            <div class="theme-name">LIGHT</div>
          </div>
          <div class="theme-btn" onclick="setTheme('amber')" id="th-amber">
            <div class="theme-swatch" style="background:#1a0e00;border:1px solid #b86000"></div>
            <div class="theme-name">AMBER</div>
          </div>
        </div>
      </div>
      <!-- Actions -->
      <div class="sys-card">
        <div class="sys-item" onclick="showConfirm('reboot')">
          <div style="display:flex;align-items:center;flex:1">
            <span class="sys-icon">🔄</span>
            <div><div class="sys-lbl">Reboot Device</div><div class="sys-sub">Restart Core2 immediately</div></div>
          </div>
          <span class="sys-chevron">›</span>
        </div>
        <div class="sys-item" onclick="showConfirm('reset')">
          <div style="display:flex;align-items:center;flex:1">
            <span class="sys-icon">⚙️</span>
            <div><div class="sys-lbl">Reset Configuration</div><div class="sys-sub">Restore all settings to default</div></div>
          </div>
          <span class="sys-chevron">›</span>
        </div>
      </div>
      <!-- Version info -->
      <div style="padding:0 14px;text-align:center">
        <div style="font-size:9px;letter-spacing:2px;color:var(--dim)">TACTICAL TIMER 2 &nbsp;|&nbsp; WEB REMOTE v3</div>
      </div>
    </div>

  </div><!-- /panels -->
</div><!-- /app -->

<!-- History detail overlay -->
<div id="det">
  <div class="det-hd">
    <button class="back" onclick="closeDet()">&#8592; BACK</button>
    <div class="det-title" id="det-title">DETAIL</div>
    <button class="det-del" onclick="delFromDet()">DELETE</button>
  </div>
  <div class="det-body" id="det-body"></div>
</div>

<!-- History list overlay (opened from CONTROL card) -->
<div id="hist-overlay" style="position:fixed;inset:0;background:var(--bg);z-index:99;display:none;flex-direction:column">
  <div class="det-hd">
    <button class="back" onclick="closeHistOverlay()">&#8592; BACK</button>
    <div class="det-title">HISTORY</div>
  </div>
  <div style="flex:1;overflow-y:auto">
    <div class="hfilter">
      <div class="hftag on" data-f="all"  onclick="setFilter('all')">ALL</div>
      <div class="hftag"   data-f="FREE"  onclick="setFilter('FREE')">FREE</div>
      <div class="hftag"   data-f="DRILL" onclick="setFilter('DRILL')">DRILL</div>
      <div class="hftag"   data-f="RO"    onclick="setFilter('RO')">RO</div>
    </div>
    <div id="hlist"><div class="hempty">Loading...</div></div>
  </div>
</div>

<!-- System confirm dialog -->
<div class="sys-confirm" id="sys-confirm">
  <div class="sys-dlg">
    <div class="sys-dlg-title" id="dlg-title">REBOOT?</div>
    <div class="sys-dlg-sub" id="dlg-sub">Core2 will restart immediately.</div>
    <div class="sys-dlg-btns">
      <button class="sys-dlg-cancel" onclick="hideConfirm()">CANCEL</button>
      <button class="sys-dlg-ok danger" id="dlg-ok" onclick="confirmAction()">CONFIRM</button>
    </div>
  </div>
</div>

<script>
// ── Utilities ────────────────────────────────────────────────
function p(n){return String(n).padStart(2,'0')}
// 時間格式化：與 Core2 fmtTime 完全一致（截斷，不四捨五入）
// Core2: snprintf("%lu.%02lu", ms/1000, (ms%1000)/10)
function fx(ms){
  ms=Math.floor(ms);
  return Math.floor(ms/1000)+'.'+(Math.floor((ms%1000)/10)).toString().padStart(2,'0');
}
function esc(s){return s?String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;'):''}

// ── Clock ────────────────────────────────────────────────────
function tick(){var n=new Date();document.getElementById('clock').textContent=p(n.getHours())+':'+p(n.getMinutes())+':'+p(n.getSeconds());document.getElementById('date').textContent=n.getFullYear()+' . '+p(n.getMonth()+1)+' . '+p(n.getDate())}
tick();setInterval(tick,1000);

// ── Tabs ─────────────────────────────────────────────────────
var TABS=['live','ctrl','sett','sys'];
function sw(t){
  TABS.forEach(function(id){document.getElementById('panel-'+id).classList.toggle('active',id===t)});
  document.querySelectorAll('.tab').forEach(function(el,i){el.classList.toggle('active',TABS[i]===t)});
}

// ── State ────────────────────────────────────────────────────
var GS_C=['idle','areyouready','waiting','beeping','going','showclear','stop'];
var GS_L=['STANDBY','ARE YOU READY','WAITING...','BEEP!','GOING','SHOW CLEAR','STOPPED'];
var MODE_N={2:'FREE',3:'DRILL',4:'DRY FIRE',5:'SPY',6:'RO',7:'HISTORY',8:'SETTINGS'};
var curGs=0,curMode=2,curRand=false,curPar=0,curSrc=1,curThr=2000;
var dShots=6,dPar=0,dPass=80,dryMs=2000,spyN=0;

function updGS(gs){
  curGs=gs;
  var c=GS_C[gs]||'idle',l=GS_L[gs]||'---';
  ['gs-free','gs-drill','gs-ro'].forEach(function(id){
    var el=document.getElementById(id);
    if(el){el.className='gs-val '+c;el.textContent=l}
  });
  document.getElementById('gsb').className='gsb gs-'+c;
  document.getElementById('gsb').textContent=l;
  var go=(gs===4),rdy=(gs===0||gs===6);
  [['bs-f','bst-f'],['bs-d','bst-d'],['bs-ro','bst-ro']].forEach(function(g){
    var s=document.getElementById(g[0]),st=document.getElementById(g[1]);
    if(s)s.disabled=!go;if(st)st.disabled=!rdy;
  });
}

function updMode(m){
  curMode=m;
  var name=MODE_N[m]||'--';
  document.getElementById('mode-badge').textContent=name;
  document.getElementById('live-mode-tag').textContent=name;
  document.querySelectorAll('.mbtn').forEach(function(el){el.classList.toggle('am',parseInt(el.dataset.m)===m)});
  var map={2:'c-free',3:'c-drill',4:'c-dry',5:'c-spy',6:'c-ro'};
  ['c-free','c-drill','c-dry','c-spy','c-ro'].forEach(function(id){document.getElementById(id).style.display='none'});
  if(map[m])document.getElementById(map[m]).style.display='block';
  // DRAW 欄只在 RO mode 顯示
  document.getElementById('sc-draw').style.display=(m===6)?'block':'none';
  // 更新 shots 分母
  document.getElementById('s-shots-max').textContent='/ '+shotsMax();
}

// ── Commands ─────────────────────────────────────────────────
function sendCmd(a){
  fetch('/cmd',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:a})}).catch(function(){});
}
// START：本地先清 log、跳到 LIVE，再送指令
// 這樣不依賴 newgame SSE 的到達時序
function startAndJump(){
  clearLog();   // 本地立即清空，不等 SSE
  sw('live');   // 跳到 LIVE
  sendCmd('start');
}
function switchMode(m){
  if(m===7){openHistOverlay();return;}  // HISTORY：開 overlay
  document.querySelectorAll('.mbtn').forEach(function(el){
    if(parseInt(el.dataset.m)===m){el.classList.add('switching');setTimeout(function(){el.classList.remove('switching')},400)}
  });
  fetch('/mode',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mode:m})})
    .then(function(){updMode(m)}).catch(function(){});
}

// ── Free / RO settings ───────────────────────────────────────
function adjPar(d){
  curPar=Math.max(0,curPar+d);
  document.getElementById('par-val').textContent=curPar?curPar:'OFF';
  document.getElementById('par-sub').textContent=curPar?(fx(curPar)+'s limit'):'Disabled';
  fetch('/cmd',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'par',parMs:curPar})}).catch(function(){});
}
function setRand(v){
  curRand=v;
  ['rnd-off','ro-rnd-off'].forEach(function(id){var el=document.getElementById(id);if(el)el.classList.toggle('on',!v)});
  ['rnd-on','ro-rnd-on'].forEach(function(id){var el=document.getElementById(id);if(el)el.classList.toggle('on',v)});
  fetch('/cmd',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'rand',value:v?1:0})}).catch(function(){});
}

// ── Drill ────────────────────────────────────────────────────
function adjD(f,d){
  if(f==='s'){dShots=Math.max(1,Math.min(10,dShots+d));document.getElementById('d-shots').textContent=dShots}
  if(f==='p'){
    dPar=Math.max(0,dPar+d);
    document.getElementById('d-par').textContent=dPar||'OFF';
    document.getElementById('d-par-sub').textContent=dPar?(fx(dPar)+'s par'):'No limit';
  }
  if(f==='q'){dPass=Math.max(0,Math.min(100,dPass+d));document.getElementById('d-pass').textContent=dPass}
  // 即時儲存（每次調整都送出）
  fetch('/drill',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({shots:dShots,parMs:dPar,passPct:dPass})}).catch(function(){});
}

// ── Dry Fire ─────────────────────────────────────────────────
var DRY_MIN=500,DRY_MAX=5000,DRY_STEP=200;
function updDryDisplay(){
  document.getElementById('bpm-ms-big').textContent=dryMs;
  document.getElementById('bpm-ref').textContent=Math.round(60000/dryMs)+' BPM';
}
function adjDry(delta){
  var next=Math.max(DRY_MIN,Math.min(DRY_MAX,dryMs+delta));
  if(next===dryMs)return;
  dryMs=next;
  updDryDisplay();
  fetch('/dryfire',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({action:'set',beatMs:dryMs})}).catch(function(){});
}
function sendDry(a){
  var body=a==='start'?{action:'start',beatMs:dryMs}:{action:'stop'};
  fetch('/dryfire',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}).catch(function(){});
  document.getElementById('dry-start').disabled=(a==='start');
  document.getElementById('dry-stop').disabled=(a==='stop');
}

// ── Spy ──────────────────────────────────────────────────────
function sendSpy(a){
  fetch('/spy',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:a})}).catch(function(){});
  if(a==='listen'){
    document.getElementById('spy-listen').disabled=true;
    document.getElementById('spy-stop').disabled=false;
    document.getElementById('gs-spy').className='gs-val going';
    document.getElementById('gs-spy').textContent='LISTENING';
  } else if(a==='stop'){
    document.getElementById('spy-listen').disabled=false;
    document.getElementById('spy-stop').disabled=true;
    document.getElementById('gs-spy').className='gs-val stop';
    document.getElementById('gs-spy').textContent='STOPPED';
  } else if(a==='clear'){
    spyN=0;
    document.getElementById('spy-n').textContent='0';
    document.getElementById('spy-t').textContent='--';
    document.getElementById('gs-spy').className='gs-val idle';
    document.getElementById('gs-spy').textContent='STANDBY';
    clearLog();
  }
}

// ── Settings ─────────────────────────────────────────────────
var pSrc=1,pThr=2000;
function applySett(){
  fetch('/setting',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({hitSource:pSrc,micThresh:pThr})})
    .then(function(){loadPresets()}).catch(function(){});
}
function setSrc(s){
  pSrc=s;
  ['esp','mic','both'].forEach(function(id,i){document.getElementById('opt-'+id).classList.toggle('on',s===i)});
  document.getElementById('mic-card').style.opacity=(s===0)?'0.4':'1';
  applySett();
}
function updThr(v){pThr=parseInt(v);document.getElementById('thr-val').textContent=pThr;applySett();}

// ── Presets ──────────────────────────────────────────────────
function loadPresets(){
  fetch('/preset').then(function(r){return r.json()}).then(function(d){
    var ul=document.getElementById('plist');ul.innerHTML='';
    var srcL=['ESP-NOW','MIC','BOTH'];
    d.presets.forEach(function(pr,i){
      var isActive=(i===d.active);
      var el=document.createElement('div');
      el.className='pitem'+(isActive?' ap':'');
      el.innerHTML=
        '<div style="flex:1;min-width:0">'+
          '<div class="pname">'+(isActive?'&#9654; ':'')+''+esc(pr.name||('PRESET '+(i+1)))+'</div>'+
          '<div class="pdetail">'+srcL[pr.src||1]+' &nbsp;&bull;&nbsp; THRESH '+pr.thresh+' RMS</div>'+
        '</div>'+
        '<div class="pchk">'+(isActive?'&#10003;':'')+'</div>';
      el.onclick=function(){if(!isActive)setPreset(i)};
      ul.appendChild(el);
    });
  }).catch(function(){});
}
function setPreset(i){
  fetch('/preset',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({active:i})})
    .then(function(){loadPresets()}).catch(function(){});
}

// ── Shot log ─────────────────────────────────────────────────
var _hitSeen={};
function clearLog(){
  _hitSeen={};
  ['L','R'].forEach(function(id){document.getElementById(id).innerHTML='<div class="empty">--- WAITING ---</div>'});
  updShotsEl(0);
  document.getElementById('lbl-total').textContent='Total';
  document.getElementById('lbl-avg').textContent='Avg Split';
  document.getElementById('s-draw').innerHTML='--<small>s</small>';
  document.getElementById('s-total').innerHTML='--<small>s</small>';
  document.getElementById('s-avg').innerHTML='--<small>s</small>';
  document.getElementById('drill-result-banner').style.display='none';
  spyN=0;document.getElementById('spy-n').textContent='0';document.getElementById('spy-t').textContent='--';
}
function updBest(t,a){
  if(t>0)document.getElementById('bt').innerHTML=t.toFixed(2)+'<small>s</small>';
  if(a>0)document.getElementById('ba').innerHTML=a.toFixed(2)+'<small>s</small>';
}
function shotsMax(){return curMode===3?dShots:10}
function updShotsEl(n){
  document.getElementById('s-shots').childNodes[0].textContent=n;
  document.getElementById('s-shots-max').textContent='/ '+shotsMax();
}
function spClass(ms){return ms<=300?'f':ms<=600?'ok':ms<=1000?'w':'s'}
function addShot(d){
  var idx=d.hitIdx;
  if(_hitSeen[idx])return;
  _hitSeen[idx]=true;
  var isMic=d.stationId===0;
  var el=fx(d.elapsed),sp=fx(d.split);
  var parFail=curPar>0&&d.elapsed>curPar;
  var col=document.getElementById(idx<=5?'L':'R');
  var ph=col.querySelector('.empty');if(ph)ph.remove();
  var r=document.createElement('div');
  r.className='srow new'+(parFail?' par-fail':'');
  r.innerHTML='<div class="rn">'+p(idx)+'</div>'+
    '<div class="srow-src"><span class="bdg '+(isMic?'mic':'esp')+'">'+(isMic?'MIC':'S'+d.stationId)+'</span></div>'+
    '<div class="tv">'+el+'<small>s</small></div>'+
    '<div class="sp '+spClass(d.split)+'">'+(d.elapsed===0&&d.split>0?'':'+')+(d.split>0?sp+'<small>s</small>':'---')+'</div>';
  col.appendChild(r);col.scrollTop=col.scrollHeight;
  updShotsEl(idx);
  // RO mode：第一槍顯示 DRAW TIME，後續顯示 TOTAL
  if(curMode===6){
    if(idx===1){
      document.getElementById('s-draw').innerHTML=el+'<small>s</small>';
    }
    document.getElementById('s-total').innerHTML=el+'<small>s</small>';
    var av=(idx>1)?fx(d.elapsed/(idx-1)):'--';
    document.getElementById('s-avg').innerHTML=av+'<small>s</small>';
  } else {
    document.getElementById('s-total').innerHTML=el+'<small>s</small>';
    var av=(d.elapsed>0&&idx>0)?fx(d.elapsed/idx):'--';
    document.getElementById('s-avg').innerHTML=av+'<small>s</small>';
  }
  if(curMode===5){spyN=idx;document.getElementById('spy-n').textContent=idx;document.getElementById('spy-t').textContent=el+'s'}
}

// ── History ──────────────────────────────────────────────────
var detPath='',histAll=[],histFilter='all';
function setFilter(f){
  histFilter=f;
  document.querySelectorAll('.hftag').forEach(function(el){el.classList.toggle('on',el.dataset.f===f)});
  renderHistList();
}
function loadHist(){
  var list=document.getElementById('hlist');
  list.innerHTML='<div class="hempty">Loading...</div>';
  fetch('/history').then(function(r){return r.json()}).then(function(d){
    histAll=d.sessions?d.sessions.slice().reverse():[];
    renderHistList();
  }).catch(function(){document.getElementById('hlist').innerHTML='<div class="hempty">Error loading</div>'});
}
function renderHistList(){
  var list=document.getElementById('hlist');
  var items=histFilter==='all'?histAll:histAll.filter(function(s){return s.mode===histFilter});
  if(!items.length){list.innerHTML='<div class="hempty">'+(histAll.length?'No '+histFilter+' sessions':'No sessions recorded')+'</div>';return}
  list.innerHTML='';
  items.forEach(function(s){
    var modeTag='<span class="hmode-tag '+(s.mode||'').toLowerCase()+'">'+esc(s.mode)+'</span>';
    var avgMs=(s.totalMs&&s.shots>1)?(s.totalMs/s.shots):0;
    var pfBadge='';
    if(s.mode==='DRILL'&&s.drill){
      var sc=s.drill.score!=null?(' '+parseFloat(s.drill.score).toFixed(0)+'%'):'';
      pfBadge='<span class="hpf '+(s.drill.passed?'p':'f')+'">'+(s.drill.passed?'PASS':'FAIL')+sc+'</span>';
    }
    var w=document.createElement('div');w.className='hitem-wrap';
    var safePath=esc(s.path);
    w.innerHTML=
      '<div class="hitem" onclick="openDet(\''+safePath+'\')">'+
        '<div class="hl">'+
          '<div class="hdate">'+esc(s.date)+'</div>'+
          modeTag+pfBadge+
        '</div>'+
        '<div class="hr">'+
          '<div class="hshots">'+s.shots+'<small>shots</small></div>'+
          '<div class="htime">'+(s.totalMs?fx(s.totalMs)+'s':'--')+'</div>'+
          (avgMs?'<div class="havg">avg '+fx(avgMs)+'s</div>':'')+
        '</div>'+
      '</div>'+
      '<button class="hdel" onclick="delSess(\''+safePath+'\',this.closest(\'.hitem-wrap\'))">DEL</button>';
    list.appendChild(w);
  });
}
function openDet(path){
  detPath=path;
  document.getElementById('det-title').textContent='LOADING...';
  document.getElementById('det-body').innerHTML='<div class="hempty">Loading...</div>';
  document.getElementById('det').classList.add('show');
  fetch('/history/detail?path='+encodeURIComponent(path))
    .then(function(r){return r.json()}).then(renderDet)
    .catch(function(){document.getElementById('det-body').innerHTML='<div class="hempty">Error loading session</div>'});
}
function closeDet(){document.getElementById('det').classList.remove('show')}
function delFromDet(){
  if(!confirm('Delete this session?'))return;
  fetch('/history/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:detPath})})
    .then(function(){closeDet();loadHist()}).catch(function(){});
}
function delSess(path,row){
  if(!confirm('Delete?'))return;
  fetch('/history/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:path})})
    .then(function(){
      histAll=histAll.filter(function(s){return s.path!==path});
      row.remove();
      if(!document.getElementById('hlist').children.length)renderHistList();
    }).catch(function(){});
}
function renderDet(d){
  var ML={2:'FREE',3:'DRILL',4:'DRY FIRE',5:'SPY',6:'RO'};
  document.getElementById('det-title').textContent=(ML[d.mode]||'FREE')+' — '+esc(d.date);
  var tot=d.totalMs?fx(d.totalMs):'--';
  var avg=(d.totalMs&&d.shots>1)?fx(d.totalMs/d.shots):'--';
  var html='';
  // Drill pass/fail banner（含達成率）
  if(d.mode===3&&d.drill){
    var scoreStr=d.score!=null?d.score.toFixed(0)+'%':'--';
    html+='<div class="drill-badge '+(d.passed?'dpas':'dfai')+'">'+
      '<div class="drill-lbl">'+(d.passed?'PASSED ✓':'FAILED ✗')+
        ' <span style="font-size:14px;opacity:.8">'+scoreStr+'</span></div>'+
      '<div class="drill-sub">'+d.drill.shots+' shots &bull; '+
        (d.drill.parMs?fx(d.drill.parMs)+'s par':'no par')+' &bull; '+d.drill.passPct+'% req</div></div>';
  }
  // Stats grid
  html+='<div class="dstats">'+
    '<div class="dstat"><div class="dslbl">Shots</div><div class="dsval">'+d.shots+'</div></div>'+
    '<div class="dstat"><div class="dslbl">Total</div><div class="dsval">'+tot+'<small>s</small></div></div>'+
    '<div class="dstat"><div class="dslbl">Avg</div><div class="dsval">'+avg+'<small>s</small></div></div>';
  if(d.bestSplit)html+='<div class="dstat"><div class="dslbl">Best Split</div><div class="dsval">'+fx(d.bestSplit)+'<small>s</small></div></div>';
  // RO Draw Time
  if(d.mode===6&&d.drawMs)
    html+='<div class="dstat" style="border-left-color:var(--yel)"><div class="dslbl">Draw</div><div class="dsval" style="color:var(--yel)">'+fx(d.drawMs)+'<small>s</small></div></div>';
  html+='</div>';
  // Preset 槍型 + Drill score
  if(d.preset||d.score!=null){
    html+='<div style="display:flex;gap:8px;margin-bottom:8px;flex-wrap:wrap">';
    if(d.preset)html+='<span style="font-size:10px;background:var(--bg3);border:1px solid var(--bord);border-radius:4px;padding:3px 8px;color:var(--cyan2);letter-spacing:1px">🎯 '+esc(d.preset)+'</span>';
    if(d.mode===3&&d.score!=null)html+='<span style="font-size:10px;background:var(--bg3);border:1px solid var(--bord);border-radius:4px;padding:3px 8px;color:'+(d.passed?'var(--grn)':'var(--red)')+';letter-spacing:1px">'+d.score.toFixed(1)+'%</span>';
    html+='</div>';
  }
  // Split bar chart
  if(d.hits&&d.hits.length>1){
    var splits=d.hits.map(function(h,i){return i===0?null:h.splitMs}).filter(function(v){return v!=null});
    var maxSplit=Math.max.apply(null,splits)||1;
    var parMs=d.drill&&d.drill.parMs?d.drill.parMs:0;
    html+='<div class="split-chart"><div class="split-chart-title">Split Distribution</div>';
    splits.forEach(function(sp,i){
      var pct=Math.round(sp/maxSplit*100);
      var cls=spClass(sp);
      var barColor=cls==='f'?'var(--grn)':cls==='ok'?'var(--cyan2)':cls==='w'?'var(--yel)':'var(--red)';
      var parPct=parMs?Math.min(100,Math.round(parMs/maxSplit*100)):0;
      html+='<div class="bar-row">'+
        '<div class="bar-label">'+(i+2)+'</div>'+
        '<div class="bar-track">'+
          '<div class="bar-fill" style="width:'+pct+'%;background:'+barColor+'">'+
            '<span class="bar-val">'+fx(sp)+'s</span>'+
          '</div>'+
          (parPct?'<div class="par-line" style="left:'+parPct+'%"></div>':'')+
        '</div></div>';
    });
    if(parMs)html+='<div style="font-size:9px;color:var(--yel);margin-top:4px;letter-spacing:1px">&#9646; par line = '+fx(parMs)+'s</div>';
    html+='</div>';
  }
  // Shot log
  if(d.hits&&d.hits.length){
    var lh='',rh='',parMs2=d.drill&&d.drill.parMs?d.drill.parMs:0;
    d.hits.forEach(function(h,i){
      var idx=i+1,el=fx(h.elapsedMs),sp=h.splitMs?fx(h.splitMs):'--';
      var parFail=parMs2&&h.elapsedMs>parMs2;
      var row='<div class="srow'+(parFail?' par-fail':'')+'">'+
        '<div class="rn">'+p(idx)+'</div>'+
        '<div class="srow-src"><span class="bdg '+(h.src===0?'mic':'esp')+'">'+(h.src===0?'MIC':'S'+h.src)+'</span></div>'+
        '<div class="tv">'+el+'<small>s</small></div>'+
        '<div class="sp '+(h.splitMs?spClass(h.splitMs):'ok')+'">'+(h.splitMs?'+'+sp:'--')+'<small>s</small></div></div>';
      if(idx<=5)lh+=row;else rh+=row;
    });
    html+='<div class="sgrid" style="margin:0">'+
      '<div class="scol"><div class="shdr"><div class="shc">#</div><div class="shc shc-src">SRC</div><div class="shc">TIME</div><div class="shc">SPL</div></div>'+lh+'</div>'+
      '<div class="scol"><div class="shdr"><div class="shc">#</div><div class="shc shc-src">SRC</div><div class="shc">TIME</div><div class="shc">SPL</div></div>'+rh+'</div></div>';
  }
  document.getElementById('det-body').innerHTML=html;
}

// ── History overlay ──────────────────────────────────────────
function openHistOverlay(){
  document.getElementById('hist-overlay').style.display='flex';
  loadHist();
}
function closeHistOverlay(){
  document.getElementById('hist-overlay').style.display='none';
}

// ── System functions ─────────────────────────────────────────
var _pendingAction='';
var CONFIRM_CFG={
  reboot:{title:'REBOOT DEVICE?',sub:'Core2 will restart immediately.\nAll unsaved state will be lost.',ok:'REBOOT'},
  reset: {title:'RESET CONFIG?', sub:'All settings will be restored to\nfactory defaults.',ok:'RESET'}
};
function showConfirm(action){
  _pendingAction=action;
  var cfg=CONFIRM_CFG[action];
  document.getElementById('dlg-title').textContent=cfg.title;
  document.getElementById('dlg-sub').textContent=cfg.sub;
  document.getElementById('dlg-ok').textContent=cfg.ok;
  document.getElementById('sys-confirm').classList.add('show');
}
function hideConfirm(){document.getElementById('sys-confirm').classList.remove('show')}
function confirmAction(){
  hideConfirm();
  if(_pendingAction==='reboot'){
    fetch('/cmd',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({action:'reboot'})}).catch(function(){});
    // 顯示重啟提示，等待重連
    document.getElementById('dot').className='dot';
    document.getElementById('clabel').textContent='REBOOTING...';
  } else if(_pendingAction==='reset'){
    fetch('/cmd',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({action:'reset_config'})})
    .then(function(){loadState()}).catch(function(){});
  }
}

// ── Theme ─────────────────────────────────────────────────────
var THEMES={
  dark: {bg:'#040d14',bg2:'#060f18',bg3:'#071420',bg4:'#0a1e2e',
         bord:'#1a5a72',cyan:'#00d9ff',cyan2:'#5ab0c8',grn:'#00ff88'},
  navy: {bg:'#080e1e',bg2:'#0c1228',bg3:'#101830',bg4:'#141e3a',
         bord:'#2a4a8a',cyan:'#4db8ff',cyan2:'#7090c8',grn:'#00ff88'},
  light:{bg:'#f0f4f8',bg2:'#e4eaf0',bg3:'#d8e2ec',bg4:'#c8d8e8',
         bord:'#8ab0c8',cyan:'#0070a8',cyan2:'#4080a0',grn:'#006830'},
  amber:{bg:'#1a0e00',bg2:'#221200',bg3:'#2a1600',bg4:'#341c00',
         bord:'#7a3a00',cyan:'#ffb000',cyan2:'#c07800',grn:'#ffd040'}
};
var curTheme='dark';
function setTheme(t){
  curTheme=t;
  var th=THEMES[t];
  var r=document.documentElement.style;
  r.setProperty('--bg',th.bg);r.setProperty('--bg2',th.bg2);
  r.setProperty('--bg3',th.bg3);r.setProperty('--bg4',th.bg4);
  r.setProperty('--bord',th.bord);r.setProperty('--cyan',th.cyan);
  r.setProperty('--cyan2',th.cyan2);r.setProperty('--grn',th.grn);
  document.querySelectorAll('.theme-btn').forEach(function(el){
    el.classList.toggle('active-theme',el.id==='th-'+t);
  });
  try{localStorage.setItem('tt2_theme',t)}catch(e){}
}
// 載入上次選的 theme
try{var _st=localStorage.getItem('tt2_theme');if(_st&&THEMES[_st])setTheme(_st);}catch(e){}

// ── loadState ────────────────────────────────────────────────
function loadState(){
  fetch('/state').then(function(r){return r.json()}).then(function(d){
    updGS(d.gameState);
    updMode(d.mode||2);
    curRand=!!d.randomDelay;curPar=d.parTimeMs||0;
    setRand(curRand);
    document.getElementById('par-val').textContent=curPar?curPar:'OFF';
    document.getElementById('par-sub').textContent=curPar?(fx(curPar)+'s limit'):'Disabled';
    // settings
    pSrc=d.hitSource||1;pThr=d.micThresh||2000;
    ['esp','mic','both'].forEach(function(id,i){document.getElementById('opt-'+id).classList.toggle('on',pSrc===i)});
    document.getElementById('mic-card').style.opacity=(pSrc===0)?'0.4':'1';
    document.getElementById('thr').value=pThr;
    document.getElementById('thr-val').textContent=pThr;
    // drill
    if(d.drill){
      dShots=d.drill.shots||6;dPar=d.drill.parMs||0;dPass=d.drill.passPct||80;
      document.getElementById('d-shots').textContent=dShots;
      document.getElementById('d-par').textContent=dPar||'OFF';
      document.getElementById('d-par-sub').textContent=dPar?(fx(dPar)+'s par'):'No limit';
      document.getElementById('d-pass').textContent=dPass;
    }
    // dryfire
    if(d.dryBeatMs){
      dryMs=Math.max(DRY_MIN,Math.min(DRY_MAX,d.dryBeatMs));
      updDryDisplay();
    }
    // best
    if(d.bestTotal>0||d.bestAvg>0)updBest(d.bestTotal,d.bestAvg);
    // replay hits
    if(d.hits&&d.hits.length){clearLog();d.hits.forEach(addShot)}
    loadPresets();
  }).catch(function(e){console.warn(e)});
}

// ── SSE ──────────────────────────────────────────────────────
if(!!window.EventSource){
  var _sseT=null;
  function connect(){
    var s=new EventSource('/events');
    s.addEventListener('open',function(){
      if(_sseT){clearTimeout(_sseT);_sseT=null}
      document.getElementById('dot').className='dot on';
      document.getElementById('clabel').textContent='CONNECTED';
      loadState();
    });
    s.addEventListener('error',function(){
      document.getElementById('dot').className='dot';
      document.getElementById('clabel').textContent='RECONNECTING...';
      s.close();if(!_sseT)_sseT=setTimeout(connect,3000);
    });
    s.addEventListener('hit',    function(e){addShot(JSON.parse(e.data))});
    s.addEventListener('gs',     function(e){updGS(parseInt(e.data))});
    s.addEventListener('newgame',function(){clearLog()});
    s.addEventListener('best',   function(e){var d=JSON.parse(e.data);updBest(d.t,d.a)});
    s.addEventListener('mode',   function(e){updMode(parseInt(e.data))});
    s.addEventListener('drybeat',function(e){
      var ms=parseInt(e.data);
      if(ms>=DRY_MIN&&ms<=DRY_MAX){dryMs=ms;updDryDisplay();}
    });
    s.addEventListener('partime',function(e){
      var d=JSON.parse(e.data);
      curPar=d.parMs||0;
      document.getElementById('par-val').textContent=curPar?curPar:'OFF';
      document.getElementById('par-sub').textContent=curPar?(fx(curPar)+'s limit'):'Disabled';
      setRand(!!d.rand);
    });
    s.addEventListener('settings',function(e){
      var d=JSON.parse(e.data);
      pThr=d.thresh;pSrc=d.src;
      document.getElementById('thr').value=pThr;
      document.getElementById('thr-val').textContent=pThr;
      ['esp','mic','both'].forEach(function(id,i){
        document.getElementById('opt-'+id).classList.toggle('on',pSrc===i);
      });
      document.getElementById('mic-card').style.opacity=(pSrc===0)?'0.4':'1';
      loadPresets();
    });
    s.addEventListener('drill',  function(e){
      var d=JSON.parse(e.data);
      dShots=d.shots;dPar=d.parMs;dPass=d.passPct;
      document.getElementById('d-shots').textContent=dShots;
      document.getElementById('d-par').textContent=dPar||'OFF';
      document.getElementById('d-par-sub').textContent=dPar?(fx(dPar)+'s par'):'No limit';
      document.getElementById('d-pass').textContent=dPass;
      if(curMode===3) document.getElementById('s-shots-max').textContent='/ '+dShots;
    });
    s.addEventListener('drillresult',function(e){
      var d=JSON.parse(e.data);
      var inner=document.getElementById('drill-result-inner');
      inner.className='drill-badge '+(d.passed?'dpas':'dfai');
      document.getElementById('drill-result-lbl').textContent=d.passed?'PASSED \u2713':'FAILED \u2717';
      var parStr=d.parMs?(fx(d.parMs)+'s par'):'no par';
      document.getElementById('drill-result-sub').innerHTML=
        d.score.toFixed(0)+'% &bull; '+d.shots+' shots &bull; '+parStr+' &bull; need '+d.passPct+'%';
      document.getElementById('drill-result-banner').style.display='block';
      sw('live');
    });
  }
  connect();
}
</script>
</body></html>)rawliteral";


// ============================================================
// Server instances
// ============================================================
static AsyncWebServer   s_server(NetCfg::WEB_PORT);
static AsyncEventSource s_events("/events");


// ============================================================
// SSE push 函式（可從任意 Task 安全呼叫）
// ============================================================

/// @brief 推播目前 GameState（整數）到所有已連線的 Web client
/**
 * @brief 推播目前 GameState 給所有 SSE 客戶端
 *
 * 事件名稱：`gs`，payload = GameState 整數值（0–6）
 */
void WebServer::sendGameState() {
  s_events.send(String(static_cast<int>(TimerCore::getState())).c_str(),
                "gs", millis());
}
/// @brief 推播新局開始事件（Web 清空 shot log）
/**
 * @brief 通知 Web 清空 shot log（新局開始）
 *
 * 事件名稱：`newgame`，payload = "1"
 */
void WebServer::sendNewGame() {
  s_events.send("1", "newgame", millis());
}

/// @brief 推播最佳紀錄更新
/// @param t  最佳總時間（秒）
/// @param a  最佳平均 split（秒）
/**
 * @brief 推播最佳紀錄更新
 *
 * 事件名稱：`best`，payload = `{"t":X.XX,"a":X.XX}`
 *
 * @param t  最佳總時間（秒）
 * @param a  最佳平均 split（秒）
 */
void WebServer::sendBest(float t, float a) {
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"t\":%.2f,\"a\":%.2f}", t, a);
  s_events.send(buf, "best", millis());
}
/**
/**
 * @brief 推播命中事件（SSE event: "hit"）
 *
 * JSON 格式：`{"hitIdx":N,"stationId":N,"elapsed":N,"split":N}`
 *
 * @param sm  SseMsg（hitIdx / stationId / elapsed / split）
 */
void WebServer::sendHit(const SseMsg& sm) {
  StaticJsonDocument<128> doc;
  doc["hitIdx"]    = sm.hitIdx;
  doc["stationId"] = sm.stationId;
  doc["elapsed"]   = sm.elapsed;
  doc["split"]     = sm.split;
  String s; serializeJson(doc, s);
  s_events.send(s.c_str(), "hit", millis());
}
/// @brief 推播模式切換事件（AppMode 整數）
/**
 * @brief 推播模式切換事件（SSE event: "mode"）
 * @param mode  目前 AppMode（整數）
 */
/**
 * @brief 推播目前 AppMode 給 Web（模式切換時呼叫）
 *
 * 事件名稱：`mode`，payload = AppMode 整數值
 *
 * @param mode  目前 AppMode
 */
void WebServer::sendMode(AppMode mode) {
  s_events.send(String(static_cast<int>(mode)).c_str(), "mode", millis());
}

/**
 * @brief 推播 DRILL 課題定義（SSE event: "drill"）
 *
 * Web 收到後更新 DRILL 設定顯示（shots / par time / pass %）。
 *
 * @param shots    目標發數
 * @param parMs    Par Time（ms）
 * @param passPct  及格百分比
 */
void WebServer::sendDrillDef(uint8_t shots, uint32_t parMs, uint8_t passPct) {
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"shots\":%d,\"parMs\":%lu,\"passPct\":%d}",
           shots, parMs, passPct);
  s_events.send(buf, "drill", millis());
}
/**
 * @brief 推播 DRILL 結果（SSE event: "drillresult"）
 *
 * Web 收到後顯示 PASSED/FAILED banner 和達成率。
 *
 * @param score    達成率（0.0–100.0）
 * @param passed   是否通過
 * @param shots    目標發數
 * @param parMs    Par Time（ms）
 * @param passPct  及格百分比
 */
void WebServer::sendDrillResult(float score, bool passed,
                                uint8_t shots, uint32_t parMs, uint8_t passPct) {
  char buf[96];
  snprintf(buf, sizeof(buf),
           "{\"score\":%.1f,\"passed\":%d,\"shots\":%d,\"parMs\":%lu,\"passPct\":%d}",
           score, passed ? 1 : 0, shots, parMs, passPct);
  s_events.send(buf, "drillresult", millis());
}
/**
 * @brief 推播 DRY FIRE beat interval（SSE event: "drybeat"）
 * @param beatMs  目前節拍間隔（ms）
 */
void WebServer::sendDryBeatMs(uint32_t beatMs) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%lu", beatMs);
  s_events.send(buf, "drybeat", millis());
}
/**
 * @brief 推播 Settings 變更（SSE event: "settings"）
 *
 * Core2 端切換 Preset 或調整 Threshold / Source 後呼叫，
 * Web 收到後即時更新 SETTINGS tab 的顯示。
 *
 * @param activePreset  目前 active preset 索引（0–4）
 * @param micThresh     目前 MIC threshold（RMS）
 * @param hitSource     目前 HitSource（0=ESP-NOW / 1=MIC / 2=BOTH）
 */
void WebServer::sendSettings(uint8_t activePreset, int16_t micThresh, uint8_t hitSource) {
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"preset\":%d,\"thresh\":%d,\"src\":%d}",
           activePreset, micThresh, hitSource);
  s_events.send(buf, "settings", millis());
}
/**
 * @brief 推播 Par Time / Random Delay 變更（SSE event: "partime"）
 *
 * Core2 PAR SET 儲存後或 Web 調整後呼叫，確保雙向同步。
 *
 * @param parMs        Par Time（ms），0 = 停用
 * @param randomDelay  Random Delay 是否啟用
 */
void WebServer::sendParTime(uint32_t parMs, bool randomDelay) {
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"parMs\":%lu,\"rand\":%d}",
           parMs, randomDelay ? 1 : 0);
  s_events.send(buf, "partime", millis());
}
/**
 * @brief C 語言介面的 sendHit 包裝函式（供 NetworkMgr 的 networkTaskFn 呼叫）
 *
 * NetworkMgr 不 include web_server.h，透過此 extern "C" 風格函式呼叫。
 *
 * @param sm  SseMsg 命中資料
 */
void webServerSendHit(const SseMsg& sm) { WebServer::sendHit(sm); }


// ============================================================
// Helper: session summary JSON object
// ============================================================

/**
 * @brief 建立 session 摘要 JSON 物件（供 /history 和 /history/detail 共用）
 *
 * 填入 path、shots、totalMs、mode 字串、date（從檔名解析）。
 *
 * @param obj   目標 JsonObject
 * @param path  SD 卡路徑（用於解析日期）
 * @param rec   GameRecord
 */
static void buildSummary(JsonObject obj, const char* path, const GameRecord& rec) {
  obj["path"]    = path;
  obj["shots"]   = rec.hit_count;
  obj["totalMs"] = rec.stop_time_ms;
  // mode string
  const char* ms = "FREE";
  switch (rec.mode) {
    case AppMode::DRILL:    ms = "DRILL";    break;
    case AppMode::SPY:      ms = "SPY";      break;
    case AppMode::RO:       ms = "RO";       break;
    case AppMode::DRY_FIRE: ms = "DRY FIRE"; break;
    default: break;
  }
  obj["mode"] = ms;
  // date from filename: /sessions/YYYYMMDD_HHMMSS.json
  const char* sl = strrchr(path, '/');
  const char* fn = sl ? sl + 1 : path;
  char db[20] = "--";
  if (strlen(fn) >= 15)
    snprintf(db, sizeof(db), "%.4s-%.2s-%.2s %.2s:%.2s",
             fn, fn+4, fn+6, fn+9, fn+11);
  obj["date"] = db;
}


// ============================================================
// init()
// ============================================================

/**
 * @brief 初始化 AsyncWebServer 並註冊所有 HTTP 路由
 *
 * 路由清單：
 *  - GET  /           → 回傳嵌入的 Web Dashboard HTML
 *  - GET  /state      → 目前遊戲狀態 JSON（gameState / mode / preset / etc.）
 *  - POST /cmd        → 遊戲控制指令（start / stop / rand / par / reboot 等）
 *  - POST /drill      → DRILL 課題設定更新
 *  - POST /dryfire    → DRY FIRE beat interval 設定
 *  - POST /spy        → SPY listen / stop / clear
 *  - POST /setting    → Threshold / HitSource 更新（同步 Preset + AppSettings）
 *  - GET  /preset     → 取得所有 Preset 列表
 *  - POST /preset     → 切換 active Preset
 *  - GET  /history/detail → 單筆 session 詳情 JSON（需在 /history 前註冊）
 *  - GET  /history    → Session 列表 JSON（最新 50 筆）
 *  - POST /history/delete → 刪除指定 session
 *  - SSE  /events     → SSE 事件流（hit / gs / mode / settings / partime / drybeat）
 *
 * @note /history/detail 必須在 /history 前面註冊，否則被前綴匹配攔截
 */
void WebServer::init() {

  // GET /
  s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", PAGE_HTML);
  });

  // GET /state
  s_server.on("/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<1024> doc;
    AppSettings cfg = PresetMgr::getEffectiveSettings();
    doc["gameState"]   = static_cast<int>(TimerCore::getState());
    doc["mode"]        = static_cast<int>(GameFSM::currentMode());
    doc["hitSource"]   = static_cast<int>(cfg.hitSource);
    doc["micThresh"]   = cfg.micThresh;
    doc["randomDelay"] = cfg.randomDelay ? 1 : 0;
    doc["parTimeMs"]   = cfg.parTimeMs;    // Free mode par（獨立）
    doc["dryBeatMs"]   = ModeDryFire::getBeatMs();
    doc["activePreset"]= PresetMgr::getActiveIdx();
    doc["bestTotal"]   = TimerCore::getBestTotal();
    doc["bestAvg"]     = TimerCore::getBestAvg();
    // Drill 課題設定：優先讀 mode_drill 的實際 _def，否則讀 AppSettings
    JsonObject drillJ = doc.createNestedObject("drill");
    const DrillDef* def = ModeDrill::getDef();
    if (def) {
      drillJ["shots"]   = def->shotCount;
      drillJ["parMs"]   = def->parTimeMs;
      drillJ["passPct"] = def->passPct;
    } else {
      drillJ["shots"]   = cfg.drillShots;
      drillJ["parMs"]   = cfg.drillParMs;
      drillJ["passPct"] = cfg.drillPassPct;
    }
    const GameRecord* rec = TimerCore::getRecord(0);
    if (rec && rec->hit_count > 0 && rec->start_time_ms > 0) {
      JsonArray arr = doc.createNestedArray("hits");
      for (uint8_t i = 0; i < rec->hit_count && i < Limits::HIT_MAX; i++) {
        JsonObject h = arr.createNestedObject();
        h["hitIdx"]    = i + 1;
        h["stationId"] = rec->hits[i].station_id;
        h["elapsed"]   = TimerCore::calcElapsed(*rec, i + 1);
        h["split"]     = TimerCore::calcSplit(*rec, i + 1);
      }
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // POST /cmd
  s_server.on("/cmd", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) { req->send(400); return; }
      const char* action = doc["action"] | "";
      GameState gs = TimerCore::getState();
      if (strcmp(action, "start") == 0) {
        if (gs == GameState::IDLE || gs == GameState::STOP) {
          UiCmdMsg m; m.cmd = WebCmd::START; IPC::sendUiCmd(m); req->send(200);
        } else { req->send(409, "application/json", "{\"error\":\"not ready\"}"); }
      } else if (strcmp(action, "stop") == 0) {
        if (gs == GameState::GOING) {
          UiCmdMsg m; m.cmd = WebCmd::STOP; IPC::sendUiCmd(m); req->send(200);
        } else { req->send(409, "application/json", "{\"error\":\"not going\"}"); }
      } else if (strcmp(action, "restart") == 0) {
        UiCmdMsg m; m.cmd = WebCmd::RESTART; IPC::sendUiCmd(m); req->send(200);
      } else if (strcmp(action, "rand") == 0) {
        bool v = ((int)doc["value"] != 0);
        TimerCore::setRandomDelay(v);
        AppSettings s = PresetMgr::getEffectiveSettings();
        s.randomDelay = v;
        HalStorage::saveSettings(s);
        req->send(200);
      } else if (strcmp(action, "par") == 0) {
        uint32_t ms = doc["parMs"] | 0;
        TimerCore::setParTime(ms);
        AppSettings s = PresetMgr::getEffectiveSettings();
        s.parTimeMs = ms;
        HalStorage::saveSettings(s);
        PresetMgr::applySettings(s);          // 同步更新記憶體快照
        // 通知 Core2 螢幕（若目前在 PAR SET 畫面則同步顯示）
        UiCmdMsg m; m.cmd = WebCmd::SET_PAR; m.parMs = ms;
        IPC::sendUiCmd(m);
        WebServer::sendParTime(ms, TimerCore::isRandomDelay());
        req->send(200);
      } else if (strcmp(action, "reboot") == 0) {
        req->send(200);
        Serial.println("[WebServer] Reboot requested via Web");
        delay(500);
        ESP.restart();
      } else if (strcmp(action, "reset_config") == 0) {
        AppSettings defaults;  // 使用 struct 預設值
        HalStorage::saveSettings(defaults);
        PresetMgr::applySettings(defaults);
        TimerCore::setRandomDelay(defaults.randomDelay);
        TimerCore::setParTime(defaults.parTimeMs);
        req->send(200);
        Serial.println("[WebServer] Config reset to defaults");
      } else {
        req->send(400, "application/json", "{\"error\":\"unknown\"}");
      }
    }
  );

  // POST /mode
  s_server.on("/mode", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<64> doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) { req->send(400); return; }
      int mi = doc["mode"] | 2;
      if (mi < 2 || mi > 8) { req->send(400); return; }
      GameFSM::requestSwitch(static_cast<AppMode>(mi));
      req->send(200);
      Serial.printf("[WebServer] Mode -> %d\n", mi);
    }
  );

  // POST /drill
  s_server.on("/drill", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) { req->send(400); return; }
      AppSettings s = PresetMgr::getEffectiveSettings();
      if (doc.containsKey("shots"))   s.drillShots   = (uint8_t)constrain((int)(doc["shots"]|6), 1, 10);
      if (doc.containsKey("parMs"))   s.drillParMs   = doc["parMs"]   | 0;
      if (doc.containsKey("passPct")) s.drillPassPct = (uint8_t)constrain((int)(doc["passPct"]|80), 0, 100);
      HalStorage::saveSettings(s);
      PresetMgr::applySettings(s);
      // 若目前在 DRILL mode，同步更新 _def（Core2 螢幕即時反映）
      ModeDrill::updateDef(s.drillShots, s.drillParMs, s.drillPassPct);
      // Drill par 同步到 TimerCore
      TimerCore::setParTime(s.drillParMs);
      req->send(200);
      Serial.printf("[WebServer] Drill saved: shots=%d parMs=%d pass=%d\n",
                    s.drillShots, s.drillParMs, s.drillPassPct);
    }
  );

  // POST /dryfire
  s_server.on("/dryfire", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<64> doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) { req->send(400); return; }
      const char* action = doc["action"] | "";
      if (strcmp(action, "set") == 0) {
        // 只設定 beatMs，不 start
        uint32_t bms = doc["beatMs"] | Timing::DRY_BEAT_DEF_MS;
        ModeDryFire::setBeatMs(bms);  // 對齊步進、儲存、SSE 推播
      } else if (strcmp(action, "start") == 0) {
        if (doc.containsKey("beatMs"))
          ModeDryFire::setBeatMs(doc["beatMs"] | Timing::DRY_BEAT_DEF_MS);
        ModeDryFire::webStart();
      } else if (strcmp(action, "stop") == 0) {
        ModeDryFire::webStop();
      }
      req->send(200);
    }
  );

  // POST /spy
  s_server.on("/spy", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<64> doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) { req->send(400); return; }
      const char* action = doc["action"] | "";
      if (strcmp(action, "listen") == 0) {
        UiCmdMsg m; m.cmd = WebCmd::START; IPC::sendUiCmd(m);
      } else if (strcmp(action, "stop") == 0) {
        UiCmdMsg m; m.cmd = WebCmd::STOP; IPC::sendUiCmd(m);
      } else if (strcmp(action, "clear") == 0) {
        UiCmdMsg m; m.cmd = WebCmd::SPY_CLEAR; IPC::sendUiCmd(m);
        WebServer::sendNewGame();  // Web 端也清空 log
      }
      req->send(200);
    }
  );

  // POST /setting
  s_server.on("/setting", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) { req->send(400); return; }
      // 同時更新 AppSettings（全域預設）
      AppSettings s = PresetMgr::getEffectiveSettings();
      if (doc.containsKey("hitSource")) s.hitSource = static_cast<HitSource>((int)doc["hitSource"]);
      if (doc.containsKey("micThresh")) s.micThresh = static_cast<int16_t>((int)doc["micThresh"]);
      HalStorage::saveSettings(s);
      // 如果有 active preset，也同步更新 preset 的 threshold / source
      uint8_t idx = PresetMgr::getActiveIdx();
      Preset pr;
      if (PresetMgr::get(idx, pr)) {
        if (doc.containsKey("hitSource")) pr.hitSource = static_cast<HitSource>((int)doc["hitSource"]);
        if (doc.containsKey("micThresh")) pr.micThresh = static_cast<int16_t>((int)doc["micThresh"]);
        PresetMgr::save(idx, pr);
      }
      PresetMgr::applySettings(s);
      // 推播 SSE 讓 Web 即時同步
      WebServer::sendSettings(PresetMgr::getActiveIdx(), s.micThresh,
                              static_cast<uint8_t>(s.hitSource));
      req->send(200);
      Serial.printf("[WebServer] Setting: src=%d thr=%d preset=%d\n",
                    (int)s.hitSource, s.micThresh, idx);
    }
  );

  // GET /preset
  s_server.on("/preset", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<512> doc;
    doc["active"] = PresetMgr::getActiveIdx();
    JsonArray arr = doc.createNestedArray("presets");
    for (uint8_t i = 0; i < Limits::PRESET_MAX; i++) {
      Preset pr; JsonObject o = arr.createNestedObject();
      if (PresetMgr::get(i, pr)) {
        o["name"] = pr.name; o["src"] = static_cast<int>(pr.hitSource); o["thresh"] = pr.micThresh;
      } else {
        char nb[16]; snprintf(nb, sizeof(nb), "PRESET %d", i+1);
        o["name"] = nb; o["src"] = 1; o["thresh"] = 2000;
      }
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // POST /preset
  s_server.on("/preset", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<64> doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) { req->send(400); return; }
      uint8_t idx = doc["active"] | 0;
      if (idx >= Limits::PRESET_MAX) { req->send(400); return; }
      PresetMgr::setActive(idx);
      // 推播 SSE 讓 Web 即時同步（切換 preset 後 thresh/src 都可能變）
      const Preset& pr = PresetMgr::getActive();
      WebServer::sendSettings(idx, pr.micThresh,
                              static_cast<uint8_t>(pr.hitSource));
      req->send(200);
      Serial.printf("[WebServer] Preset -> %d\n", idx);
    }
  );

  // GET /history
  // GET /history/detail（必須在 /history 前面，否則被攔截）
  s_server.on("/history/detail", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("path")) { req->send(400); return; }
    String path = req->getParam("path")->value();
    GameRecord rec; DrillDef drill;
    if (!HalStorage::loadSession(path.c_str(), rec, &drill)) {
      req->send(404, "application/json", "{\"error\":\"not found\"}"); return;
    }
    StaticJsonDocument<2048> doc;
    // 直接填欄位（不用 buildSummary，避免 as<JsonObject>() 的 reference 問題）
    doc["path"]    = path;
    doc["shots"]   = rec.hit_count;
    doc["totalMs"] = rec.stop_time_ms;
    doc["mode"]    = static_cast<int>(rec.mode);
    // date from filename
    const char* sl = strrchr(path.c_str(), '/');
    const char* fn = sl ? sl + 1 : path.c_str();
    char db[20] = "--";
    if (strlen(fn) >= 15)
      snprintf(db, sizeof(db), "%.4s-%.2s-%.2s %.2s:%.2s",
               fn, fn+4, fn+6, fn+9, fn+11);
    doc["date"] = db;

    // 從 SD 讀取額外欄位（preset / drawMs / score / passed）
    char presetBuf[Limits::NAME_LEN] = {};
    uint32_t drawMs = 0; float score = 0.0f; bool passed2 = false;
    if (HalStorage::readSessionExtra(path.c_str(), presetBuf, sizeof(presetBuf),
                                     drawMs, score, passed2)) {
      if (presetBuf[0]) doc["preset"] = presetBuf;
      if (drawMs > 0)   doc["drawMs"] = drawMs;
      if (rec.mode == AppMode::DRILL) {
        doc["score"]  = score;
        doc["passed"] = passed2 ? 1 : 0;
      }
    }

    unsigned long bestSplit = ULONG_MAX;
    JsonArray hits = doc.createNestedArray("hits");
    for (uint8_t i = 0; i < rec.hit_count && i < Limits::HIT_MAX; i++) {
      JsonObject h = hits.createNestedObject();
      h["src"]       = rec.hits[i].station_id;
      h["elapsedMs"] = rec.hits[i].hit_time_ms;
      unsigned long sp = (i == 0) ? rec.hits[i].hit_time_ms
                                  : rec.hits[i].hit_time_ms - rec.hits[i-1].hit_time_ms;
      h["splitMs"] = sp;
      if (i > 0 && sp < bestSplit) bestSplit = sp;
    }
    if (bestSplit != ULONG_MAX) doc["bestSplit"] = bestSplit;
    if (rec.mode == AppMode::DRILL && drill.shotCount > 0) {
      JsonObject dj = doc.createNestedObject("drill");
      dj["shots"] = drill.shotCount; dj["parMs"] = drill.parTimeMs; dj["passPct"] = drill.passPct;
      if (!doc.containsKey("passed")) {
        uint8_t cnt = 0;
        for (uint8_t i = 0; i < rec.hit_count; i++)
          if (drill.parTimeMs == 0 || rec.hits[i].hit_time_ms <= drill.parTimeMs) cnt++;
        doc["passed"] = rec.hit_count > 0 && (cnt * 100 / rec.hit_count) >= drill.passPct;
      }
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // GET /history
  s_server.on("/history", HTTP_GET, [](AsyncWebServerRequest* req) {
    static char paths[50][Limits::SD_PATH_LEN];
    uint16_t count = 0;
    HalStorage::listSessions(paths, 50, count);
    StaticJsonDocument<3072> doc;
    JsonArray arr = doc.createNestedArray("sessions");
    for (uint16_t i = 0; i < count; i++) {
      GameRecord rec; DrillDef drill;
      if (!HalStorage::loadSession(paths[i], rec, &drill)) continue;
      JsonObject o = arr.createNestedObject();
      buildSummary(o, paths[i], rec);
      if (rec.mode == AppMode::DRILL) {
        JsonObject dj = o.createNestedObject("drill");
        dj["parMs"] = drill.parTimeMs; dj["shots"] = drill.shotCount;
        // 從 SD 讀取 passed / score
        char pb[Limits::NAME_LEN] = {};
        uint32_t dm = 0; float sc = 0.0f; bool psd = false;
        HalStorage::readSessionExtra(paths[i], pb, sizeof(pb), dm, sc, psd);
        dj["passed"] = psd ? 1 : 0;
        dj["score"]  = sc;
      }
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // POST /history/delete
  s_server.on("/history/delete", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) { req->send(400); return; }
      const char* path = doc["path"] | "";
      if (!path[0]) { req->send(400); return; }
      bool ok = HalStorage::deleteSession(path);
      req->send(ok ? 200 : 404);
      Serial.printf("[WebServer] Delete: %s -> %s\n", path, ok?"OK":"FAIL");
    }
  );

  // SSE /events
  s_events.onConnect([](AsyncEventSourceClient* c) {
    c->send("connected", "status", millis(), 10000);
    float bt = TimerCore::getBestTotal(), ba = TimerCore::getBestAvg();
    if (bt > 0.0f) {
      char buf[48]; snprintf(buf, sizeof(buf), "{\"t\":%.2f,\"a\":%.2f}", bt, ba);
      c->send(buf, "best", millis());
    }
    c->send(String(static_cast<int>(TimerCore::getState())).c_str(), "gs", millis());
    c->send(String(static_cast<int>(GameFSM::currentMode())).c_str(), "mode", millis());
  });

  s_server.addHandler(&s_events);
  s_server.begin();
  Serial.printf("[WebServer] Started on port %u\n", NetCfg::WEB_PORT);
}
