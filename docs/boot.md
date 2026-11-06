# PW-H1 warm boot

## 状態

診断ログで確認したV690_H1の配置を使う。Linuxからinitramfsへの到達は実機確認済み。
専用予約領域への移動で画面破損が解消したとの実機報告あり。実機結果に応じた周辺機器・M4通信の調整が必要。

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
| Linux用LCDバッファ（1 MiB予約） | `66e00000` | `66e00000` |
| 移行中の純正OS用LCDバッファ | `60a00008` | `60a00008` |
| PEGが表示していたバッファ（実機ログ） | `60aca810` | `60aca810` |

下位16 MiBはLinuxのRAMから外し、既存フレームバッファと低位の純正OS領域を残す。
加えて `66e00000`〜`66efffff` を `reserved-memory` / `no-map` とし、Linuxの割り当てとリニアマッピングから除外する。
simplefbだけが表示に必要な `0xc8280` バイトを明示的にマップする。
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
5. 周辺機器を停止した後、画面を専用予約領域へコピーしてLCDIFの表示先を切り替える。LCDIFは動かしたまま、ARMタイマー割り込みを無効化する。
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
- 画面はsimplefbと小さなLCDIF設定ドライバを組み合わせる。LCDのタイミングやMIPI初期化は変更しない。
  クロック初期化時のLCDIF・DSIゲート停止をPW-H1のみ抑止し、設定ドライバが両クロックを取得・保持する。
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
割り込み停止後は、周辺機器を止める前に現在の画面を一旦 `60a00008` へコピーし、
停止完了後に `66e00000` へ移す。いずれもキャッシュをcleanしてNEXT_BUFを設定し、
CUR_BUFの切り替えを100 ms以内に確認してから進む。
対応するのは実機ログと同じ480×854・16 bit・DOTCLK/BYPASS_COUNTモードと、上記3つのバッファのみ。
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
diag 9はUSB失敗時にPHY・usbmisc・USB本体・ChipIdea子デバイスのドライバー登録状態を短い行で表示する。
DEBUG_FSで依存デバイス待ちの理由も取得し、通常のusbcore登録メッセージを診断画面から除く。
表示は48文字で折り返した先頭22行。全内容は `/run/usb-report.log` に保存する。

## 表示の安定化（display 1）

実機のCTRL1=`020f0300`ではRECOVERY_ON_UNDERFLOWが無効だった。
参照カーネルの `drivers/video/fbdev/mxsfb.c` に合わせ、CTRL1のbit 24を有効にし、
CTRL2のOUTSTANDING_REQSを16要求（値4）に設定する。純正OSの補助表示レイヤーのDMAも停止する。
ローダーでLinuxへ移る前に設定し、Linux側でも設定・読戻しを確認する。
連続表示中のRUNを33 msごとに操作する処理は削除し、LCDIFとDSIのクロックを保持する。
BUXBOOT.TXTには変更前のCTRL2・AS_CTRL・AS_BUF・AS_NEXT_BUFも記録する。
これらは表示の欠け・時間経過でのノイズへの対策だが、実機で改善するかは未確認。

display 2では表示用RAMを純正OSの領域から分離する。
ローダーの進捗表示・アセンブリの色帯・simplefbの表示先を `66e00000` に統一し、
カーネル・DTB・ペイロードと重ならないことを静的assertで検査する。
読み込み時には予約領域の恒等マッピングとDTBの表示先・予約サイズ・no-mapを検査する。
古いBOOT.BINや予約設定が異なるものは起動しないため、AppMain.binとBOOT.BINを一緒に更新すること。
純正OSのバッファをLinuxが再利用していたと確認できたわけではなく、共有の影響を排除するための変更。
display 2で画面の破損が解消し、initramfsの `~ #` プロンプトまで表示されたとの実機報告あり。

## USB PHYタイムアウト

実機ではPHY・usbmisc・imx_usbは登録済みで、ChipIdeaのPHY初期化が `-110` で失敗している。
diag 10ではU-Bootの `scg_enable_usb_pll()` と同じ15 µsのレギュレーター安定待ちを追加する。
失敗時はPLLロックとPHYリセットを区別し、SIC・CTRL・SIM_MISC_CTRL0の値を記録する。
PLL分周比や制御権の設定は、値を確認せずに変更しない。

diag 10ではPHYリセットのタイムアウトらしき表示後に白画面へ戻る実機報告があった。
diag 11はPW-H1に限り、U-Bootと同じPLL出力ENABLE（bit 13）も設定する。
また、初期化失敗・終了時にUSB PLLを停止しない。純正OSから引き継いだ表示クロックへの
影響を切り分けるための暫定措置で、PLLと白画面の因果関係はまだ未確認。
表示用予約RAMはdisplay 2のまま維持する。PLLを保持するためUSB終了時の省電力は未対応。

diag 11でも白いノイズが再発したため、PLL停止回避だけでは解消していない。
diag 12は表示の切り分け用としてUSBコントローラー・PHY・usbmiscをDTで無効化する。
chosenの `brainux,display-only` によりinitramfsもUSB設定とgetty起動を省略する。
表示用RAM・LCD設定は変更しない。ローダーによる引き継ぎ前のUSB DMA停止は維持する。
この版はUSBシリアルを利用できず、実機の表示安定性も未確認。
復帰時は3ノードのstatusとchosenのマーカーを一緒に戻すこと。

diag 12でも画面破損が再発した。display 2での改善報告も再現性未確認として扱う。
diag 13ではUSB無効を維持し、`nohlt` でCPUのアイドルをポーリングにする。
`CONFIG_CPU_IDLE=n` でもARMの既定idle処理は `cpu_do_idle()` を呼ぶため、WFIは残る。
本カーネルは `CONFIG_GENERIC_IDLE_POLL_SETUP=y` で `nohlt` を受理する。
純正OSから引き継いだ低電力設定の影響を切り分ける目的であり、原因確定ではない。
CPUが待機しないため消費電力・発熱が増える可能性があり、短時間の確認に限る。

## キーボード状態の読取り（diag 14）

Reflexの入力は純正OSのPEGイベントなので、Linux移行後は利用できない。
V690_H1のsystem.binにはM4用Thumbコードがあり、ファイル位置0x175c00を
M4アドレス0x1ffd0000へ対応させると、次のキータスクを追跡できる。

- 0x17d2b4: 0x1fff8080を状態構造体として使うタスク。
- 0x183406: キーを2回走査し、3バイトが一致する場合だけ結果を採用。
- 0x17d440: 無押下なら状態+8の3バイトをゼロにする。
- 0x17d462: 押下なら状態+8へ3バイトをコピーする。

`pwh1-keys.c` はキータスクの先頭8バイトを照合し、状態RAMを読取り専用で公開する。
GPIO・MU・M4のコードやデータへ書き込まない。Linux移行後もM4のスキャンが継続するか、
A7からこのRAMを読み取れるか、各キー番号がどの文字に対応するかは実機未確認。
このため、まだLinux inputイベントには変換しない。
initramfsは `raw_keys` の3バイトと状態値を表示する。A・Q・Enter・Shiftを個別に
押して離し、値とゼロへ戻るかを記録する。キー対応を確定後、inputドライバーへ移行する。
この診断中はシェルを開始しない。nohltとUSB無効を維持する。

diag 14の実機結果はA=`01`、Q=`11`、Shift=`2e`、Enter=`25`、電源=`22`。
AとShiftは離すとゼロへ戻り、A+Qは `01 11 00`。stateは押下時2、その後3となる。
3キー同時押しは `00 00 00` になった。電源断との当初の報告は電池切れとの訂正あり。
電源キーの `22` は残留したため、Linuxの入力イベントへ変換しない。

diag 15は20 ms周期のLinux inputポーリングで、2回連続して同じ3バイトを読んだら
押下・解放を反映する。M4が3バイトを別々に書く途中の値を採用しにくくするための措置で、
短い押下を取りこぼす可能性がある。3キー押下のゼロも通常の全解放として処理する。
A〜Zは01〜1aの連番と推定して対応付け、Shiftを文字より先に通知する。
Enterも対応し、リピートはLinux inputに任せる。未確認キーは無視する。
診断ループを削除して本体画面のシェルを再開する。`raw_keys` は引き続き参照可能。
ビルドと抽出した実装の一時的なassertチェックで、押下・解放・同時押し・Shiftの通知順・
電源キー無視を確認した。Linuxコンソールでの実入力はまだ実機未確認。
