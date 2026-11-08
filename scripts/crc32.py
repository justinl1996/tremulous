import zlib
import sys

if __name__ == "__main__":
    print(zlib.crc32(open(sys.argv[1],'rb').read())&0xffffffff)

