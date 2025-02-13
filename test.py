#!/usr/bin/env python3

import ctypes

clib = ctypes.CDLL("./libcompskyplayaudio.so")
clib.initFFMPEG.restype = ctypes.c_voidp
clib.uninitFFMPEG.argtypes = [ctypes.c_voidp]
clib.playAudio.argtypes = [ctypes.c_voidp, ctypes.POINTER(ctypes.c_char), ctypes.c_float, ctypes.c_float, ctypes.c_float]

globalvars = clib.initFFMPEG()
if globalvars == 0:
	raise Exception("C library error: init_all() returned true")

if __name__ == "__main__":
	import sys
	
	for fp in sys.argv[1:]:
		clib.playAudio(globalvars, fp.encode(), 0.0, 0.0, 1.0)
	
	clib.uninitFFMPEG(globalvars)