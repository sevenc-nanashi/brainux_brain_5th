# PW-H1 warm boot

## 状態

診断ログで確認したV690_H1の配置を使う。起動ローダーとカーネルはビルド検証済みだが、
Linux起動成功はまだ確認していない。実機結果に応じた周辺機器・M4通信の調整が必要。

## メモリ

| 用途 | 純正OS側VA | PA |
| --- | --- | --- |
| ローダーコード | `90000000` | `67100000` |
| 起動用ページテーブル（16 KiB） | `90500000` | `67600000` |
| ローダーdata/bss | `90504000` | `67604000` |
| BOOT.BIN一時置き場（9 MiB） | `90600000` | `67700000` |
| Linux Imageコピー先 | MMU無効後に書く | `61008000` |
| DTBコピー先 | MMU無効後に書く | `67000000` |
| Linuxに渡すRAM | — | `61000000`〜`67ffffff` |
| Linux用LCDバッファ | `60a00008` | `60a00008` |
| PEGが表示していたバッファ（実機ログ） | `60aca810` | `60aca810` |

下位16 MiBはLinuxのRAMから外し、既存フレームバッファと低位の純正OS領域を残す。
これはM4がアクセスする全メモリを特定したことを意味しない。M4の継続動作・heartbeat・電源制御は未検証。
追加アプリに割り当てられた領域以外へ、純正OSの通常実行中にカーネルを書き込まない。

## 読み込みと検証

AppContext.exec_pathから起動フォルダーを求め、BOOT.BINを読み取り専用で開く。
読み取り完了とclose成功後に、サイズ・SHA-256・DTBを検証する。
SHA-256は転送破損検出であり、署名や作成者の認証ではない。

BOOT.BINのヘッダーは64バイト。magic `BUXBOOT1`、Image長とDTB長（LE u32）、
ヘッダー以降のSHA-256（32バイト）、予約領域16バイト（0）の順。
Imageの直後を8バイト境界へ0埋めし、DTBを置く。
Imageは4 KiB〜9 MiBからヘッダー64バイト・DTB予約64 KiBを引いたサイズまでの非圧縮ARM Linux Image、DTBは40バイト〜64 KiB。
zImageやU-BootのuImageを入れない。initramfsはImageに組み込む。

## 移行

1. Enterの後にBが押された時点でペイロードを再検証し、独自のshort-descriptorテーブルを作る。
2. IRQ/FIQを止める。USDHCの転送終了を待ち、USDHCとUSBをリセットする。
3. eDMAの要求を止め、稼働チャネルの停止を待つ。未対応のCAAM/GPUが有効なら停止する。
4. A7側のwatchdogをunlock・無効化し、結果を確認する。MUの割り込みをマスクする。
5. LCDIFは動かしたまま、ARMタイマー割り込みを無効化する。表示バッファはLinuxへ渡すRAM範囲の外に保持する。
   実機ではRUNが100 ms以内に0へ戻らなかったため、Reflexと同様にRUNの完了待ち・強制クリアは行わない。
6. スタックを使わないARMコードで、CLIDR/CCSIDRに従って全データキャッシュ階層をclean/invalidateする。
7. 独自テーブルへ切り替える。RAMの実行可能な恒等マッピングと元のコードVAを両方用意し、
   物理アドレス側へ分岐してからMMU・D/I-cache・分岐予測を無効化する。
8. ImageとDTBをそれぞれのPAへコピーする。SVCモードへ切り替え、
   r0=0、r1=~0、r2=DTBのPAでImageへ分岐する。

手順2以降は純正OSへ戻らない。待機のタイムアウト・解除不可watchdogなどはpanic停止する。
タイムアウトは既存Reflexの1 MHz TSTMRを使う。
Linuxのタイマーはi.MX7ULP TPMで、CNTFRQ=0を周波数として採用しない。

根拠は [Linux ARM boot protocol](https://docs.kernel.org/arch/arm/booting.html)、
参照カーネルの `arch/arm/mm/cache-v7.S`、`drivers/dma/fsl-edma-common.h`、
`drivers/watchdog/imx7ulp_wdt.c`、`drivers/clk/imx/clk-imx7ulp.c`。
実機のレジスター状態と各停止処理はまだ照合していない。

## Linux側

- 既存PW-H1 DTをそのまま流用せず、SoC共通DTSIから作る。
  1 GiBのRAM・192 MiB CMA・EVKのRPMsg領域を使わない。
- warm boot専用compatibleで純正OSに合わないi.MX7ULPのPM初期化・cpuidle・NMI処理を回避する。
- SDは純正OSのpinmuxと電源を引き継ぐ。内蔵eMMC、GPU、CAAM、RPMsg、省電力は無効。
- 画面はsimplefbと小さなLCDIF RUN更新ドライバを組み合わせる。LCDのタイミングやMIPI初期化は変更しない。
  クロック初期化時のLCDIF・DSIゲート停止をPW-H1のみ抑止し、更新ドライバがLCDIFクロックを取得・有効化する。
- USB peripheralとACMで初期シェルを提供する。SD上の既存Brainuxへswitch_rootする経路を含む。

## 未確認・未対応

実機での制御移行、SDの配線・電源、USBの再接続、LCD更新、watchdogの停止、
M4の監視動作・メモリ共有は要検証。nativeキーボード・タッチ・音声・バッテリー・電源断は未対応。
PEGコールバックをLinuxへ持ち越して入力を使うことはしていない。
エラーが出た場合はその画面と、出力されていればUSBのカーネルログで原因を絞る。
Bを押した後はステータス欄に入力受理・ペイロード検証・`Boot 1/6`〜`6/6` を表示する。
各表示後に250 ms待ち、周辺機器を停止する前に画面転送の時間を設ける（RUNの完了待ちではない）。
進捗はLCDIF CUR_BUFが指す実際の表示先へ描画する。実機ではPEGの表示先が
`60aca810` であり、Reflexの固定アドレス `60a00008` への描画は見えていなかった。
割り込み停止後、周辺機器を止める前に現在の画面を `60a00008` へコピーしてキャッシュをcleanし、
NEXT_BUFを同アドレスへ設定する。CUR_BUFの切り替えを100 ms以内に確認してから進む。
対応するのは実機ログと同じ480×854・16 bit・DOTCLK/BYPASS_COUNTモードと、上記2つのバッファのみ。
割り込みを止める前に、同じフォルダーの `BUXBOOT.TXT` を上書き保存する。
LCDIFのCTRL・CTRL1・TRANSFER_COUNT・CUR_BUF・NEXT_BUFとLCDIFクロックの設定値を記録する。
diag 4ではwatchdogのPCCと、クロック有効時のみCS・TOVAL・WINも記録する。
watchdogはUPDATE=1なら解除シーケンスを実行し、UPDATE=0なら初回設定として停止を試みる。
後者も実際にEN=0となったことを確認し、設定済みのロックなどで停止できなければアドレスとCS値を表示して停止する。
UPDATE=0が必ずしも停止不能を意味しない点は
[U-Bootのi.MX7ULP初期化](https://github.com/u-boot/u-boot/blob/master/arch/arm/mach-imx/mx7ulp/soc.c)に基づく。
1はストレージ、2はUSB、3はDMA、4はwatchdog、5はMUの停止開始、6はMMU切り替え直前を示す。
6の表示だけではカーネルへ到達したとは判断できない。
diag 5ではアセンブリの移行処理から画面左端に16 px幅の色帯を順に追加する。
赤は全キャッシュのclean/invalidate後、青はMMU無効化後、緑はImage・DTBのコピー後でLinuxへ分岐する直前。
各段階は250 ms表示する。緑もLinux本体の実行成功を保証するものではない。
診断ログにはDSIのPCC (`40b300a4`) も含める。
カーネル側のdiag 6は本体画面のみをconsoleにし、未確認のUARTは無効にする。
`initcall_debug` と `loglevel=8` で初期化処理名・戻り値を表示し、PF_PACKET登録後の停止位置を確認する。
initramfsは開始時とUSB設定・コントローラー待機・bind完了時に画面へ出力する。
USB接続時にノイズが出て抜くと消える実機報告があるが、その原因はまだ特定できていない。
diag 7ではUSB PHYのSIMレジスター取得に必要な `CONFIG_MFD_SYSCON` を有効化する。
これが無効だとPHYのprobeは `-ENOTSUPP` で失敗する。ビルド時にも必須設定として検査する。
USB設定は独立した `sh -e` で実行し、失敗時はUSB関連のカーネルログを画面に表示する。
diag 8では大量の出力を避けるため `initcall_debug` を外し、カーネルのconsole loglevelを4にする。
initramfs開始後は緊急メッセージ以外を画面に流さず、USB設定後に画面を消去して結果を表示する。
USB設定とgettyの詳細はRAM上の `/run/usb-setup.log`・`/run/usb-getty.log` に保存し、カーネルログは `dmesg` で取得できる。
これはログで結果が流れる問題への対処であり、USB接続時の表示ノイズを解決したことは実機未確認。
