#!/usr/bin/env python3

from ctypes import *
import argparse
import enum
import platform
import pyaudio
import queue
import sys
import threading
import time

audio_queue = queue.Queue(maxsize=32)
hd_program = 0


def audio_worker():
    p = pyaudio.PyAudio()
    try:
        index = p.get_default_output_device_info()["index"]
        stream = p.open(format=pyaudio.paInt16,
                        channels=2,
                        rate=44100,
                        output_device_index=index,
                        output=True)
    except OSError:
        print("No audio output device available.")
        stream = None

    while True:
        samples = audio_queue.get()
        if samples is None:
            break
        if stream:
            stream.write(samples)
        audio_queue.task_done()

    if stream:
        stream.stop_stream()
        stream.close()
    p.terminate()


class EventType(enum.Enum):
    NRSC5_EVENT_LOST_DEVICE = 0
    NRSC5_EVENT_IQ = 1
    NRSC5_EVENT_SYNC = 2
    NRSC5_EVENT_LOST_SYNC = 3
    NRSC5_EVENT_MER = 4
    NRSC5_EVENT_BER = 5
    NRSC5_EVENT_HDC = 6
    NRSC5_EVENT_AUDIO = 7
    NRSC5_EVENT_ID3 = 8
    NRSC5_EVENT_SIG = 9
    NRSC5_EVENT_LOT = 10


class Audio(Structure):
    _fields_ = [
        ("program", c_uint),
        ("data", POINTER(c_char)),
        ("count", c_size_t),
    ]


class EventUnion(Union):
    _fields_ = [
        ("audio", Audio),
    ]


class Event(Structure):
    _fields_ = [
        ("event", c_uint),
        ("u", EventUnion),
    ]


def callback(evt, opaque):
    evt = evt.contents
    type = EventType(evt.event)
    if type == EventType.NRSC5_EVENT_AUDIO:
        audio = evt.u.audio
        audio_event(audio.program, audio.data[:audio.count * 2])


def audio_event(program, samples):
    if program == hd_program:
        audio_queue.put(samples)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Receive NRSC-5 signals.')
    parser.add_argument("-r")
    parser.add_argument("freq", type=float)
    parser.add_argument("program", type=int)
    args = parser.parse_args()

    if args.freq < 10000:
        args.freq *= 1e6
    hd_program = args.program

    if platform.system() == "Windows":
        lib_name = "libnrsc5.dll"
    elif platform.system() == "Linux":
        lib_name = "libnrsc5.so"
    elif platform.system() == "Darwin":
        lib_name = "libnrsc5.dylib"
    else:
        print("Unsupported platform: " + platform.system())
        exit(1)
    libnrsc5 = cdll.LoadLibrary(lib_name)
    radio = c_void_p()

    if args.r == "-":
        f = sys.stdin.buffer
        libnrsc5.nrsc5_open_pipe(byref(radio))
    elif args.r:
        f = open(args.r, "rb")
        libnrsc5.nrsc5_open_pipe(byref(radio))
    else:
        if libnrsc5.nrsc5_open(byref(radio), 0, 0):
            print("Open device failed.")
            exit(1)
        libnrsc5.nrsc5_set_frequency(radio, c_float(args.freq))

    callback_func = CFUNCTYPE(None, POINTER(Event), c_void_p)(callback)
    libnrsc5.nrsc5_set_callback(radio, callback_func, None)

    libnrsc5.nrsc5_start(radio)

    audio_thread = threading.Thread(target=audio_worker)
    audio_thread.start()

    try:
        if args.r:
            while True:
                data = f.read(32768)
                if len(data) == 0:
                    break
                libnrsc5.nrsc5_pipe_samples(radio, data, (len(data) // 4) * 4)
        else:
            while True:
                input()
    except KeyboardInterrupt:
        print("Stopping...")
        pass

    libnrsc5.nrsc5_stop(radio)
    libnrsc5.nrsc5_close(radio)

    audio_queue.put(None)
    audio_thread.join()
