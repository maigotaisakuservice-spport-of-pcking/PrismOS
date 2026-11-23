#!/usr/bin/env python3
"""PrismOS — ML-KEM-512 の公開実装ベクトル生成。

cryptography ライブラリには ML-KEM-512 が無いため、
PQClean の Python バインディング (pqcrypto) を参照実装に
して、双方向の既知応答ベクトルを作る:

  1. pqcrypto で鍵対と (ct_a, ss_a) を作る
     → C 側の decaps(dk, ct_a) == ss_a がテストになる。
  2. 小さな C ハーネス (scripts/pq-kat512.c、カーネルと
     同じ shell/mlkem.c をリンク) で固定した m に対する
     C 側の (ct_b, ss_b) を作り、pqcrypto の decaps が
     ss_b を返すこともここで確かめる
     → C 側の encaps(ek, m) == (ct_b, ss_b) がテストになる。

出力は tests/pq_vectors_512.h。既存の pq-vectors.py
(ML-KEM-768/1024 と ML-DSA、更新鍵を含む) には触らない。

Usage: pip install --break-system-packages pqcrypto
       python3 scripts/pq-kat512.py
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

try:
    from pqcrypto.kem.ml_kem_512 import keygen, encaps, decaps
except ImportError:
    print("pqcrypto is required: pip install pqcrypto")
    return_code = 1
    raise SystemExit(return_code)


def to_c_array(name, data):
    lines = ["static const uint8_t %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    pk, sk = keygen()
    ct_a, ss_a = encaps(pk)
    assert bytes(decaps(sk, ct_a)) == bytes(ss_a)

    # Fixed encaps message coins for the C-side vector.
    m = bytes(range(0xA0, 0xC0))

    harness_src = os.path.join(ROOT, "scripts", "pq-kat512.c")
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "pq-kat512")
        cc = ["cc", "-std=c11", "-O2", "-I", os.path.join(ROOT, "shell"),
              harness_src,
              os.path.join(ROOT, "shell", "mlkem.c"),
              os.path.join(ROOT, "shell", "sha3.c"),
              "-o", exe]
        r = subprocess.run(cc, capture_output=True, text=True)
        if r.returncode != 0:
            print("harness build failed:\n%s" % r.stderr)
            return 1
        # Mode "encaps": stdin = pk || m; stdout = ct || ss.
        r = subprocess.run([exe, "encaps"], input=bytes(pk) + m,
                           capture_output=True)
        if r.returncode != 0 or len(r.stdout) != 768 + 32:
            print("harness encaps failed")
            return 1
        ct_b = r.stdout[:768]
        ss_b = r.stdout[768:]
        # The reference implementation must agree with the
        # C encaps: decaps of our ciphertext yields our ss.
        if bytes(decaps(sk, ct_b)) != ss_b:
            print("CROSS CHECK FAILED: pqcrypto decaps disagrees "
                  "with the C encaps")
            return 1
        # And the C decaps must agree with the reference
        # encaps (checked again in the host test, but fail
        # early here with a clear message).
        r = subprocess.run([exe, "decaps"], input=bytes(sk) + bytes(ct_a),
                           capture_output=True)
        if r.returncode != 0 or r.stdout != bytes(ss_a):
            print("CROSS CHECK FAILED: C decaps disagrees with "
                  "pqcrypto encaps")
            return 1

    lines = []
    lines.append("/* PrismOS — ML-KEM-512 参照ベクトル (自動生成)。")
    lines.append(" * scripts/pq-kat512.py が PQClean (pqcrypto) を")
    lines.append(" * 参照実装として作った。直接編集しないこと。 */")
    lines.append("#ifndef PRISM_PQ_VECTORS_512_H")
    lines.append("#define PRISM_PQ_VECTORS_512_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(to_c_array("PQ_KEM512_PK", bytes(pk)))
    lines.append(to_c_array("PQ_KEM512_SK", bytes(sk)))
    lines.append("/* 参照実装の encaps: decaps で照合する。 */")
    lines.append(to_c_array("PQ_KEM512_CT_REF", bytes(ct_a)))
    lines.append(to_c_array("PQ_KEM512_SS_REF", bytes(ss_a)))
    lines.append("/* 固定 m での C 実装の encaps: 参照実装の")
    lines.append(" * decaps と一致することを生成時に確認済み。 */")
    lines.append(to_c_array("PQ_KEM512_M", m))
    lines.append(to_c_array("PQ_KEM512_CT_C", ct_b))
    lines.append(to_c_array("PQ_KEM512_SS_C", ss_b))
    lines.append("")
    lines.append("#endif /* PRISM_PQ_VECTORS_512_H */")
    out = os.path.join(ROOT, "tests", "pq_vectors_512.h")
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s (cross-checks passed)" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
