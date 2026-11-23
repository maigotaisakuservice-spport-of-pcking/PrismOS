/* PrismOS — 横断検索の一致判定 (純粋関数)
 *
 * 検索窓 (F8) がアプリ名・ファイル名・設定項目を絞り込むときに
 * 使う。クエリが名前の中に部分一致 (大文字小文字を区別しない)
 * すれば 1 を返す。空のクエリはすべてに一致する。
 */
#ifndef PRISM_SEARCH_H
#define PRISM_SEARCH_H

/* query が name の中に含まれるか (大文字小文字を区別しない)。
 * どちらかが NULL なら 0。 */
int search_match(const char *query, const char *name);

#endif /* PRISM_SEARCH_H */
