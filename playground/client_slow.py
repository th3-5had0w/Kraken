from pyautogui import screenshot
import socket
import io
from pwn import *
from time import sleep, time_ns

IP = '127.0.0.1'
PORT = 8000
FILE = 'sample.txt'
CHUNKSIZE = 4096

def init():
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM, 0)
    client.connect((IP, PORT))
    return client

def _file(file, sock : socket.socket):
    cnt = 0
    f = os.open(file, os.O_RDONLY)
    finf = os.fstat(f)
    print(finf.st_size)
    if file == 'client_side.py':
        return
    with open(file, 'rb') as f:
        for _ in range((finf.st_size // CHUNKSIZE) + 1):
            if (cnt < 2):
                sleep(1)
            cnt+=1
            sock.send(f.read(CHUNKSIZE))
    sock.close()


def main():
    sock = init()
    _file(FILE, sock)

if __name__=='__main__':
    main()
