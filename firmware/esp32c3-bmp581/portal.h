/*
 * portal.h - 設定モード (Wi-Fi アクセスポイント + 設定ページ)
 *
 * スマホから、感度などの設定を変更するためのモードです。
 * 入る・出るは、再起動で行います (boot.h)。通常モードでは、
 * このモジュールは使われず、Wi-Fi は初期化されません。
 *
 * キャプティブポータル:
 *   スマホは Wi-Fi に接続すると、インターネットに繋がるかを確認するために、
 *   connectivitycheck.gstatic.com (Android) や captive.apple.com (iOS) などへ
 *   アクセスする。DNS で全ての名前をこの基板に向け、ここで設定ページへ
 *   転送する (または HTML を直接返す) と、「ログインが必要な Wi-Fi」と判断され、
 *   接続を維持したまま設定ページが開く。何も応答しないと、スマホが登録済みの
 *   別の Wi-Fi へ自動で切り替えてしまう。
 *
 * Web サーバには、非同期のもの (ESPAsyncWebServer + AsyncTCP) を使う。
 * iPhone は、接続の直後に複数の接続を同時に開くため、標準の同期 WebServer では
 * 応答が遅れて、別の Wi-Fi へ切り替わってしまった。
 * ハンドラは、ループとは別のタスクで動く。
 */
#pragma once

#include <stdint.h>

void portalBegin();                       // アクセスポイント、DNS、Web サーバを起動する
void portalService(uint32_t nowMs);       // ループから毎回呼ぶ (DNS、電池電圧の監視、終了の処理)
bool portalHasClient();                   // スマホが接続しているか

// 設定ページの /status に出す、最新のセンサ値を渡す
void portalSetSensor(float hPa, float tempC);
