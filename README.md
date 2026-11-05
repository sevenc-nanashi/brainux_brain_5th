# Brainux 第5世代移植

対象は **PW-H1 / V690_H1**。現時点では **Linuxは起動しない**。
このリポジトリには、移植に先立って実機のCPU・アドレス変換を調べる
`BrainuxProbe` を実装している。起動ローダー・Linuxカーネル・rootfsは未実装／未同梱。

既存のPW-H1用Linuxデバイスツリーには、実機資料と一致しないRAM容量などが残っている。
調査結果と次の実装箇所は [docs/bringup.md](docs/bringup.md) に記録した。

## ビルド

隣の `../brain_5th_hack` にあるReflexとその起動コードを再利用する。
参照時のコミットは `0b9553617cdc5dba191cac4a1b5fc55ba8b31d36`。
path依存なので、そのディレクトリの実際の内容でビルドされる。
ファームウェアのバイナリ自体はビルドに不要。

Rust（edition 2024対応）、GNU Make、`llvm-objcopy-20` が必要。

```sh
rustup target add armv7a-none-eabi
make check
make
```

Cargoやobjcopyの場所は `make CARGO=/path/to/cargo OBJCOPY=/path/to/llvm-objcopy` で指定できる。
整形は `make fmt`。ARM向けclippyとリンク時の領域・エントリーポイント検査を行う。

## 実機での採取

1. `target/APP/BrainuxProbe/` の `AppMain.bin` を、本体に配置済みのアプリへ上書きする。
   新規配置の場合は空の `index.din` と `FONT-LICENSE.txt` も同梱する。
2. 純正OSの追加アプリから `BrainuxProbe` を起動する。
3. `Saved: BUXPROBE.TXT next to app` と表示されたら、Escで終了する。
4. アプリと同じフォルダーの `BUXPROBE.TXT` を取り出す。
   本体・SDのどちらから起動しても、配置先に保存する。

左右キー／ページキーでレポートを閲覧できる。Enterは同じ採取結果の保存を再試行する。
保存先は `AppContext.exec_path` から取得する。statでディレクトリーかファイルかを判別し、アプリのフォルダーへ保存する。
open・write・flush・closeの失敗は画面に表示する。
再起動や再保存では、同名の `BUXPROBE.TXT` を上書きする。

採取する内容:

- CPSR、MIDR、SCTLR、TTBCR、TTBR0/1、DACR、CTR、CLIDR、VBAR、CNTFRQ。
- アプリ、スタック、ヒープ、フレームバッファ、周辺機器の仮想→物理アドレス変換。
- 静的解析のRAM別名・追加アプリ領域について、4 KiB単位の変換結果の照合。

変換はATS1CPRで調べ、対象アドレスのメモリ・MMIOは読み出さない。
PARと割り込みマスクは各照会の前後で保存・復元する。
MMU無効またはlong-descriptor形式の場合は、その旨を記録し変換照会を行わない。
RAM容量やMMIOデバイスの応答可否を検査するものではない。
watchdog・DMAの稼働状態やCortex-M4との通信状態は、まだ採取していない。

MMU・キャッシュの無効化、Linuxへのジャンプは行わない。
アプリの配置先には診断結果の `BUXPROBE.TXT` を書き込む。
純正OSの描画・ファイル・メモリ管理はReflexを通じて利用する。
ファームウェア版の自動判定はなく、V690_H1以外には対応していない。

## 検証状況

ARM向けビルド・clippy・ELF構造を確認済み。実機でのログ採取・保存にも成功した。
[採取結果](docs/bringup.md#実機ログ)では、全ページのアドレス変換が静的解析と一致している。
アプリ終了後の復帰とLinux起動は未確認。
以降はDMA停止、キャッシュ同期、MMU切替、カーネル起動を実機ログ付きで順に検証する。
