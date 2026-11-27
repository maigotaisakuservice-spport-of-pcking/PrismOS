#!/usr/bin/env python3
"""PrismOS — 更新マニフェストに ML-DSA-44 で署名する。

使い方:
    python3 scripts/pq-sign.py <manifest-file> [sig-file]

秘密鍵は scripts/pq-private-key.pem から読む (GitHub には
絶対に上げないこと)。署名は 2420 バイトのバイナリで、
省略時は <manifest-file>.sig に書く。
更新サーバはマニフェストと .sig を同じ場所に置くこと。
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    manifest_path = argv[1]
    sig_path = argv[2] if len(argv) > 2 else manifest_path + ".sig"
    key_path = os.path.join(HERE, "pq-private-key.pem")
    if not os.path.exists(key_path):
        print("missing private key:", key_path)
        print("run: python3 scripts/pq-vectors.py")
        return 1
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import mldsa

    with open(key_path, "rb") as f:
        pem = f.read()
    sk = serialization.load_pem_private_key(pem, password=None)
    if not isinstance(sk, mldsa.MLDSA44PrivateKey):
        print("private key is not ML-DSA-44")
        return 1
    with open(manifest_path, "rb") as f:
        manifest = f.read()
    sig = sk.sign(manifest)
    if len(sig) != 2420:
        print("unexpected signature length:", len(sig))
        return 1
    with open(sig_path, "wb") as f:
        f.write(sig)
    print("signed %s -> %s (%d bytes)" % (manifest_path, sig_path, len(sig)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
