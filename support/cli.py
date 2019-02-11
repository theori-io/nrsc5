#!/usr/bin/env python3

import argparse
import logging
import nrsc5
import os
import pyaudio
import queue
import sys
import threading
import wave


class NRSC5CLI:
    def __init__(self):
        self.parse_args()
        self.audio_queue = queue.Queue(maxsize=64)
        self.device_condition = threading.Condition()

    def parse_args(self):
        parser = argparse.ArgumentParser(description="Receive NRSC-5 signals.")
        input_group = parser.add_mutually_exclusive_group()
        parser.add_argument("-q", action="store_true")
        parser.add_argument("-l", metavar="log-level", type=int, default=1)
        parser.add_argument("-d", metavar="device-index", type=int, default=0)
        parser.add_argument("-p", metavar="ppm-error", type=int, default=0)
        parser.add_argument("-g", metavar="gain", type=float)
        input_group.add_argument("-r", metavar="iq-input")
        parser.add_argument("-w", metavar="iq-output")
        parser.add_argument("-o", metavar="wav-output")
        parser.add_argument("--dump-hdc", metavar="hdc-output")
        parser.add_argument("--dump-aas-files", metavar="directory")
        input_group.add_argument("frequency", nargs="?", type=float)
        parser.add_argument("program", type=int)
        self.args = parser.parse_args()

        if self.args.frequency and self.args.frequency < 10000:
            self.args.frequency *= 1e6

    def run(self):
        logging.basicConfig(level=self.args.l * 10,
                            format="%(asctime)s %(levelname)-5s %(filename)s:%(lineno)d: %(message)s",
                            datefmt="%H:%M:%S")
        if self.args.q:
            logging.disable(logging.CRITICAL)

        radio = nrsc5.NRSC5(lambda type, evt: self.callback(type, evt))

        if self.args.r:
            iq_input = sys.stdin.buffer if self.args.r == "-" else open(self.args.r, "rb")
            radio.open_pipe()
        else:
            radio.open(self.args.d, self.args.p)
            radio.set_frequency(self.args.frequency)
            if self.args.g:
                radio.set_gain(self.args.g)

        if self.args.w:
            self.iq_output = sys.stdout.buffer if self.args.w == "-" else open(self.args.w, "wb")

        if self.args.o:
            self.wav_output = wave.open(self.args.o, "wb")
            self.wav_output.setnchannels(2)
            self.wav_output.setsampwidth(2)
            self.wav_output.setframerate(44100)
        else:
            audio_thread = threading.Thread(target=self.audio_worker)
            audio_thread.start()

        if self.args.dump_hdc:
            self.hdc_output = sys.stdout.buffer if self.args.dump_hdc == "-" else open(self.args.dump_hdc, "wb")

        radio.start()

        try:
            if self.args.r:
                while True:
                    data = iq_input.read(32768)
                    if len(data) == 0:
                        break
                    radio.pipe_samples(data[:(len(data) // 4) * 4])
            else:
                with self.device_condition:
                    self.device_condition.wait()
        except KeyboardInterrupt:
            logging.info("Stopping...")
        except nrsc5.NRSC5Error as e:
            logging.error(e)

        radio.stop()
        radio.close()

        if self.args.r:
            iq_input.close()

        if self.args.w:
            self.iq_output.close()

        if self.args.o:
            self.wav_output.close()
        else:
            self.audio_queue.put(None)
            audio_thread.join()

        if self.args.dump_hdc:
            self.hdc_output.close()

    def audio_worker(self):
        p = pyaudio.PyAudio()
        try:
            index = p.get_default_output_device_info()["index"]
            stream = p.open(format=pyaudio.paInt16,
                            channels=2,
                            rate=44100,
                            output_device_index=index,
                            output=True)
        except OSError:
            logging.warn("No audio output device available.")
            stream = None

        while True:
            samples = self.audio_queue.get()
            if samples is None:
                break
            if stream:
                stream.write(samples)
            self.audio_queue.task_done()

        if stream:
            stream.stop_stream()
            stream.close()
        p.terminate()

    def adts_header(self, length):
        length += 7
        return bytes([
            0xff,
            0xf1,
            0x5c,
            0x80 | (length >> 11),
            (length >> 3) & 0xff,
            0x1f | ((length & 0x07) << 5),
            0xfc
        ])

    def callback(self, type, evt):
        if type == nrsc5.EventType.LOST_DEVICE:
            logging.info("Lost device")
            with self.device_condition:
                self.device_condition.notify()
        elif type == nrsc5.EventType.IQ:
            if self.args.w:
                self.iq_output.write(evt.data)
        elif type == nrsc5.EventType.SYNC:
            logging.info("Got Sync")
        elif type == nrsc5.EventType.LOST_SYNC:
            logging.info("Lost sync")
        elif type == nrsc5.EventType.MER:
            logging.info("MER: {:.1f} dB (lower), {:.1f} dB (upper)".format(evt.lower, evt.upper))
        elif type == nrsc5.EventType.BER:
            logging.info("BER: {:.6f}".format(evt.cber))
        elif type == nrsc5.EventType.HDC:
            if self.args.dump_hdc:
                if evt.program == self.args.program:
                    self.hdc_output.write(self.adts_header(len(evt.data)))
                    self.hdc_output.write(evt.data)
        elif type == nrsc5.EventType.AUDIO:
            if evt.program == self.args.program:
                if self.args.o:
                    self.wav_output.writeframes(evt.data)
                else:
                    self.audio_queue.put(evt.data)
        elif type == nrsc5.EventType.ID3:
            if evt.program == self.args.program:
                if evt.title:
                    logging.info("Title: " + evt.title)
                if evt.artist:
                    logging.info("Artist: " + evt.artist)
                if evt.album:
                    logging.info("Album: " + evt.album)
                if evt.genre:
                    logging.info("Genre: " + evt.genre)
                if evt.ufid.owner:
                    logging.info("Unique file identifier: {} {}".format(evt.ufid.owner, evt.ufid.id))
                if evt.xhdr.param:
                    logging.info("XHDR: param={} mime={} lot={}"
                                 .format(evt.xhdr.param, evt.xhdr.mime, evt.xhdr.lot))
        elif type == nrsc5.EventType.SIG:
            for service in evt:
                logging.info("SIG Service: type={} number={} name={}"
                             .format(service.type, service.number, service.name))
                for component in service.components:
                    if component.type == nrsc5.ComponentType.AUDIO:
                        logging.info("  Audio component: id={} port={} type={} mime={}"
                                     .format(component.id, component.audio.port,
                                             component.audio.type, component.audio.mime))
                    elif component.type == nrsc5.ComponentType.DATA:
                        logging.info("  Data component: id={} port={} service_data_type={} type={} mime={}"
                                     .format(component.id, component.data.port,
                                             component.data.service_data_type,
                                             component.data.type, component.data.mime))
        elif type == nrsc5.EventType.LOT:
            logging.info("LOT file: port={:04X} lot={} name={} size={} mime={}"
                         .format(evt.port, evt.lot, evt.name, len(evt.data), evt.mime))
            if self.args.dump_aas_files:
                path = os.path.join(self.args.dump_aas_files, evt.name)
                with open(path, "wb") as f:
                    f.write(evt.data)


if __name__ == "__main__":
    cli = NRSC5CLI()
    cli.run()
