from pyautogui import screenshot
import socket
from pwn import *

server_addr, server_port = ()
_server = remote(server_addr, server_port)
# server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
# server.connect((server_addr, server_port))

def _screen():
    pass

def _file():
    pass

def wait_command():
    cmd = server.recvline(0).decode()
    if cmd == "SCREEN":
        _screen()
    elif cmd == "FILE":
        _file()


