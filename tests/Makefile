crosstest: faudio_tests.exe

faudio_tests.exe: xaudio2.c
	x86_64-w64-mingw32-gcc -I$(WINESRC)/include/ -I$(WINE64SRC)/include/ -o $@ xaudio2.c -lole32

faudio_tests: xaudio2.c
	gcc -I../src/ -L$(FA64ROOT)/lib/ -o $@ xaudio2.c -lFAudio
