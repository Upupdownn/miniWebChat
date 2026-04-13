import socket
import struct
import json
import argparse
import threading
import time


# ========================
# 协议封装
# ========================
def pack_message(data_dict):
    json_str = json.dumps(data_dict)
    body = json_str.encode('utf-8')
    header = struct.pack('!I', len(body))  # 4字节长度头（大端）
    return header + body


def recv_exact(sock, n):
    data = b''
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def recv_message(sock):
    header = recv_exact(sock, 4)
    if not header:
        return None
    length = struct.unpack('!I', header)[0]
    body = recv_exact(sock, length)
    if not body:
        return None
    return json.loads(body.decode('utf-8'))


# ========================
# 接收线程
# ========================
def recv_loop(sock):
    while True:
        try:
            msg = recv_message(sock)
            if msg is None:
                print("[INFO] 服务器断开连接")
                break

            print("\n[RECV]", json.dumps(msg, indent=2))

        except Exception as e:
            print("[ERROR] 接收异常:", e)
            break


# ========================
# 主逻辑
# ========================
def main():
    parser = argparse.ArgumentParser(description="Chat Client Test")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("user", help="用户名")
    parser.add_argument("password", help="密码")

    args = parser.parse_args()

    # 连接服务器
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((args.host, args.port))

    print("[INFO] 已连接服务器")

    # 启动接收线程
    t = threading.Thread(target=recv_loop, args=(sock,), daemon=True)
    t.start()

    # ========================
    # 发送登录
    # ========================
    login_req = {
        "type": "login_req",
        "request_id": "1",
        "timestamp": int(time.time()),
        "data": {
            "username": args.user,
            "password": args.password
        }
    }

    sock.sendall(pack_message(login_req))
    print("[INFO] 已发送登录请求")

    # ========================
    # 聊天循环
    # ========================
    req_id = 2
    while True:
        try:
            text = input(">>> ")

            if text.strip() == "":
                continue

            chat_req = {
                "type": "chat_req",
                "request_id": str(req_id),
                "timestamp": int(time.time()),
                "data": {
                    "text": text
                }
            }

            sock.sendall(pack_message(chat_req))
            req_id += 1

        except KeyboardInterrupt:
            print("\n[INFO] 退出客户端")
            sock.close()
            break


if __name__ == "__main__":
    main()