#!/usr/bin/env python3

import ctypes

clib = ctypes.CDLL("./libcompskyplayaudio.so")
clib.init_all.restype = ctypes.c_int
clib.uninit_all.restype = ctypes.c_int
clib.playAudio.argtypes = [ctypes.POINTER(ctypes.c_char), ctypes.c_float, ctypes.c_float, ctypes.c_float]
if clib.init_all():
	raise Exception("C library error: init_all() returned true")

if __name__ == "__main__":
	import sys
	
	for fp in sys.argv[1:]:
		clib.playAudio(fp.encode(), 0.0, 0.0, 0.1)