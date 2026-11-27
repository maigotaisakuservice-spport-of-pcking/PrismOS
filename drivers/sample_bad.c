/*
 * 負例フィクスチャ: API 表にないシンボルを呼ぶパッケージ。
 * ローダが -3 (未解決シンボル) で拒否することを確かめるために
 * 使う。導入されてはいけない。
 */
int nonexistent_api_fn(int x);

int prism_driver_init(int api_version) {
    (void)api_version;
    return nonexistent_api_fn(1);
}
