import socket
import os
import io

IP = '127.0.0.1'
PORT_FAST = 8001
PORT_SLOW = 8002
FILE = 'sample.txt'
CHUNKSIZE = 4096

def init():
    client_fast = socket.socket(socket.AF_INET, socket.SOCK_STREAM, 0)
    client_fast.connect((IP, PORT_FAST))
    client_slow = socket.socket(socket.AF_INET, socket.SOCK_STREAM, 0)
    client_slow.connect((IP, PORT_SLOW))
    return (client_fast, client_slow)

def _file(file, sock : socket.socket):
    f = os.open(file, os.O_RDONLY)
    finf = os.fstat(f)
    print(finf.st_size)
    with open(file, 'rb') as fd:
        for _ in range((finf.st_size // CHUNKSIZE) + 1):
            sock.send(fd.read(CHUNKSIZE))
    sock.close()
    os.close(f)


def main():
    sz = input("How much? ")
    os.system("head -c {} /dev/urandom > {}".format(sz, FILE))
    os.system("md5sum {}".format(FILE))
    fast, slow = init()
    _file(FILE, fast)
    _file(FILE, slow)

if __name__=='__main__':
    main()
