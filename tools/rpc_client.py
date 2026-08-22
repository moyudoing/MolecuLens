#!/usr/bin/env python3
"""MolecuLens · tools/rpc_client.py —— ZeroMQ RPC 客户端示例

规格来源：主方案 §11.2（REP 套接字 tcp://127.0.0.1:5555，仅回环 + token；
          协议 {method, params, id}，暴露与嵌入式 Python 同一方法面）。

用法：
    python rpc_client.py load data/demo.pdb
    python rpc_client.py repr --type cartoon --selection "chain A"
    python rpc_client.py snapshot --out shot.png
"""
import argparse
import json
import sys

try:
    import zmq
except ImportError:
    print("需要 pyzmq: pip install pyzmq", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    ap = argparse.ArgumentParser(description="MolecuLens RPC 客户端")
    ap.add_argument("--port", type=int, default=5555)
    ap.add_argument("--token", default="")
    ap.add_argument("method")
    ap.add_argument("params", nargs="*", default=[])
    args = ap.parse_args()

    ctx = zmq.Context()
    sock = ctx.socket(zmq.REQ)
    sock.connect(f"tcp://127.0.0.1:{args.port}")

    params = {}
    for kv in args.params:
        if "=" in kv:
            k, v = kv.split("=", 1)
            try:
                params[k] = json.loads(v)
            except json.JSONDecodeError:
                params[k] = v

    req = {"method": args.method, "params": params, "id": 1}
    if args.token:
        req["token"] = args.token
    sock.send_json(req)
    resp = sock.recv_json()
    print(json.dumps(resp, indent=2, ensure_ascii=False))
    return 0 if resp.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
