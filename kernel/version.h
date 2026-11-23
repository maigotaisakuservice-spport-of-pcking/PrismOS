/* PrismOS — バージョン (1 か所で持つ)。
 *
 * PRISM_VERSION はユーザーに見える製品表記 (カーネルのバナー、
 * シェルの ver / about、設定の更新ページ、組み込みの README /
 * VERSION ファイル)。製品版からは年・半期表記にする
 * (27H1 = 2027 年上半期の製品版)。
 *
 * PRISM_VERSION_ORDER は更新マニフェストとの新旧比較専用の
 * 内部順序バージョン。マニフェストの版数は数値系列
 * (0.2.0 → 0.3.0 → ...) で発行されるため、表示ラベルを
 * そのまま数値比較すると新旧が逆転してしまう。比較には
 * 必ずこちらを使い、表示には使わない。将来の製品更新を
 * 出すときは、マニフェストの版数に合わせてこちらも進める。
 *
 * 版を上げる場合はこのファイルだけ変え、テストの期待値も
 * 合わせること。
 */
#ifndef PRISM_VERSION_H
#define PRISM_VERSION_H

#define PRISM_VERSION "27H1"
#define PRISM_VERSION_ORDER "0.2.0"

#endif /* PRISM_VERSION_H */
