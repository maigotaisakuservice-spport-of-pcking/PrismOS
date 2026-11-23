#!/usr/bin/env python3
"""PrismOS 耐量子暗号の参照ベクトル生成 (フェーズ18)。

Python の `cryptography` ライブラリ (ML-KEM / ML-DSA の
参照実装) を使い、PrismOS 自前の C 実装と突き合わせるための
テストベクトルを作る。出力は次の 3 つ。

  * tests/pq_vectors.h — ホストテスト (tests/test_pqc.c) が
    読むベクトル。ML-KEM-768/1024 の鍵ペア・カプセル化・
    共有鍵、ML-DSA-44 の鍵ペア・署名、SHA3/SHAKE の既知値。
  * shell/pq_key.h — 更新署名用の公開鍵 (ML-DSA-44、1312
    バイト) を C 配列として埋め込む。OS が更新マニフェスト
    の署名を検証するときに使う。
  * scripts/pq-private-key.pem — 更新署名用の秘密鍵。
    絶対に GitHub へ push しないこと (.gitignore 済み)。
    更新サーバ側の署名 (scripts/pq-sign.py) が使う。

使い方: python3 scripts/pq-vectors.py
  (PyPI の cryptography >= 44 が必要。実行環境に無ければ
   pip install --break-system-packages "cryptography>=44")
"""

import hashlib
import os
import sys

try:
    from cryptography.hazmat.primitives.asymmetric import mlkem, mldsa
    from cryptography.hazmat.primitives import serialization
except ImportError:
    print("MISSING: cryptography >= 44 が必要です")
    sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def to_c_array(name, data):
    lines = ["static const uint8_t %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def to_c_hex(name, data):
    return 'static const char %s[] =\n    "%s";' % (
        name, data.hex())


def kem_vectors():
    out = []
    for label, cls in (("768", mlkem.MLKEM768PrivateKey),
                       ("1024", mlkem.MLKEM1024PrivateKey)):
        sk = cls.generate()
        pk = sk.public_key()
        # encapsulate() は (共有鍵 32 バイト, 暗号文) の順で
        # 返す。長さで見分ける。
        a, b = pk.encapsulate()
        ss, ct = (a, b) if len(a) == 32 else (b, a)
        # 秘密鍵は 64 バイトのシード (d || z)。展開した鍵は
        # C 側の keygen がシードから作る (公開鍵と一致する
        # ことが、keygen の正しさの確認にもなる)。
        out.append((label, sk, pk, ct, ss))
    return out


def dsa_vectors():
    out = []
    for i, msg in enumerate((b"", b"hello prism", b"PrismOS update 0.3.0")):
        sk = mldsa.MLDSA44PrivateKey.generate()
        pk = sk.public_key()
        sig = sk.sign(msg)
        out.append((i, msg, sk, pk, sig))
    return out


def shake_vectors():
    out = []
    for msg in (b"", b"abc",
                b"PrismOS post-quantum crypto test vector 0123456789"):
        out.append((msg,
                    hashlib.sha3_256(msg).digest(),
                    hashlib.sha3_512(msg).digest(),
                    hashlib.shake_128(msg).digest(64),
                    hashlib.shake_256(msg).digest(64)))
    return out


def main():
    kem = kem_vectors()
    dsa = dsa_vectors()
    shakes = shake_vectors()

    lines = []
    lines.append("/* PrismOS — 耐量子暗号の参照ベクトル (自動生成)。")
    lines.append(" * scripts/pq-vectors.py が Python の cryptography")
    lines.append(" * ライブラリ (ML-KEM / ML-DSA の参照実装) から作る。")
    lines.append(" * 直接編集しないこと。作り直すときは")
    lines.append(" *   python3 scripts/pq-vectors.py")
    lines.append(" * を実行する。")
    lines.append(" */")
    lines.append("#ifndef PRISM_PQ_VECTORS_H")
    lines.append("#define PRISM_PQ_VECTORS_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    for label, sk, pk, ct, ss in kem:
        lines.append("/* ML-KEM-%s (秘密鍵は 64 バイトのシード d||z) */" % label)
        lines.append(to_c_array("PQ_KEM%s_PK" % label,
                                pk.public_bytes_raw()))
        lines.append(to_c_array("PQ_KEM%s_SEED" % label,
                                sk.private_bytes_raw()))
        lines.append(to_c_array("PQ_KEM%s_CT" % label, ct))
        lines.append(to_c_array("PQ_KEM%s_SS" % label, ss))
        lines.append("")
    for i, msg, sk, pk, sig in dsa:
        lines.append("/* ML-DSA-44 ベクトル %d (msg %d バイト、"
                     "秘密鍵は 32 バイトのシード) */" % (i, len(msg)))
        lines.append(to_c_array("PQ_DSA%d_PK" % i,
                                pk.public_bytes_raw()))
        lines.append(to_c_array("PQ_DSA%d_SK" % i,
                                sk.private_bytes_raw()))
        lines.append(to_c_array("PQ_DSA%d_SIG" % i, sig))
        lines.append(to_c_hex("PQ_DSA%d_MSG" % i, msg))
        lines.append("")
    for i, (msg, s256, s512, sh128, sh256) in enumerate(shakes):
        lines.append("/* SHA3/SHAKE ベクトル %d (msg %d バイト) */" % (i, len(msg)))
        lines.append(to_c_hex("PQ_SHA_MSG%d" % i, msg))
        lines.append(to_c_hex("PQ_SHA3_256_%d" % i, s256))
        lines.append(to_c_hex("PQ_SHA3_512_%d" % i, s512))
        lines.append(to_c_hex("PQ_SHAKE128_%d" % i, sh128))
        lines.append(to_c_hex("PQ_SHAKE256_%d" % i, sh256))
        lines.append("")
    lines.append("#endif /* PRISM_PQ_VECTORS_H */")
    lines.append("")
    with open(os.path.join(ROOT, "tests", "pq_vectors.h"), "w") as f:
        f.write("\n".join(lines))
    print("wrote tests/pq_vectors.h")

    # 更新署名用の鍵ペア (ML-DSA-44)。公開鍵だけを OS に埋め込む。
    sk = mldsa.MLDSA44PrivateKey.generate()
    pk = sk.public_key()
    pk_raw = pk.public_bytes_raw()
    key_lines = []
    key_lines.append("/* PrismOS — 更新署名用の公開鍵 (ML-DSA-44)。")
    key_lines.append(" * scripts/pq-vectors.py が作る。秘密鍵は")
    key_lines.append(" * scripts/pq-private-key.pem にあり、OS には")
    key_lines.append(" * 入れない。更新マニフェストの署名検証に使う。")
    key_lines.append(" */")
    key_lines.append("#ifndef PRISM_PQ_KEY_H")
    key_lines.append("#define PRISM_PQ_KEY_H")
    key_lines.append("")
    key_lines.append("#include <stdint.h>")
    key_lines.append("")
    key_lines.append("#define PQ_UPDATE_PK_LEN 1312")
    key_lines.append("")
    key_lines.append(to_c_array("PQ_UPDATE_PK", pk_raw))
    key_lines.append("")
    key_lines.append("#endif /* PRISM_PQ_KEY_H */")
    key_lines.append("")
    with open(os.path.join(ROOT, "shell", "pq_key.h"), "w") as f:
        f.write("\n".join(key_lines))
    print("wrote shell/pq_key.h")

    pem = sk.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption())
    with open(os.path.join(HERE, "pq-private-key.pem"), "wb") as f:
        f.write(pem)
    print("wrote scripts/pq-private-key.pem (DO NOT PUSH)")


if __name__ == "__main__":
    main()
