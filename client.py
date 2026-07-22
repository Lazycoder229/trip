import socket
import threading
import sys

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("localhost", 9000))

def reader():
    while True:
        data = s.recv(4096)
        if not data:
            print("\n[disconnected]")
            break
        print(data.decode(errors="replace"), end="")

threading.Thread(target=reader, daemon=True).start()

while True:
    msg = input()
    s.sendall((msg + "\n").encode())