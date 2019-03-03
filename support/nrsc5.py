from ctypes import *
import collections
import enum
import math
import platform


class EventType(enum.Enum):
    LOST_DEVICE = 0
    IQ = 1
    SYNC = 2
    LOST_SYNC = 3
    MER = 4
    BER = 5
    HDC = 6
    AUDIO = 7
    ID3 = 8
    SIG = 9
    LOT = 10
    SIS = 11


class ServiceType(enum.Enum):
    AUDIO = 0
    DATA = 1


class ComponentType(enum.Enum):
    AUDIO = 0
    DATA = 1


class MIMEType(enum.Enum):
    PRIMARY_IMAGE = 0xBE4B7536
    STATION_LOGO = 0xD9C72536
    NAVTEQ = 0x2D42AC3E
    HERE_TPEG = 0x82F03DFC
    HERE_IMAGE = 0xB7F03DFC
    HD_TMC = 0xEECB55B6
    HDC = 0x4DC66C5A
    TEXT = 0xBB492AAC
    JPEG = 0x1E653E9C
    PNG = 0x4F328CA0
    TTN_TPEG_1 = 0xB39EBEB2
    TTN_TPEG_2 = 0x4EB03469
    TTN_TPEG_3 = 0x52103469
    TTN_STM_TRAFFIC = 0xFF8422D7
    TTN_STM_WEATHER = 0xEF042E96


class Access(enum.Enum):
    PUBLIC = 0
    RESTRICTED = 1


class ServiceDataType(enum.Enum):
    NON_SPECIFIC = 0
    NEWS = 1
    SPORTS = 3
    WEATHER = 29
    EMERGENCY = 31
    TRAFFIC = 65
    IMAGE_MAPS = 66
    TEXT = 80
    ADVERTISING = 256
    FINANCIAL = 257
    STOCK_TICKER = 258
    NAVIGATION = 259
    ELECTRONIC_PROGRAM_GUIDE = 260
    AUDIO = 261
    PRIVATE_DATA_NETWORK = 262
    SERVICE_MAINTENANCE = 263
    HD_RADIO_SYSTEM_SERVICES = 264
    AUDIO_RELATED_DATA = 265


class ProgramType(enum.Enum):
    UNDEFINED = 0
    NEWS = 1
    INFORMATION = 2
    SPORTS = 3
    TALK = 4
    ROCK = 5
    CLASSIC_ROCK = 6
    ADULT_HITS = 7
    SOFT_ROCK = 8
    TOP_40 = 9
    COUNTRY = 10
    OLDIES = 11
    SOFT = 12
    NOSTALGIA = 13
    JAZZ = 14
    CLASSICAL = 15
    RHYTHM_AND_BLUES = 16
    SOFT_RHYTHM_AND_BLUES = 17
    FOREIGN_LANGUAGE = 18
    RELIGIOUS_MUSIC = 19
    RELIGIOUS_TALK = 20
    PERSONALITY = 21
    PUBLIC = 22
    COLLEGE = 23
    SPANISH_TALK = 24
    SPANISH_MUSIC = 25
    HIP_HOP = 26
    WEATHER = 29
    EMERGENCY_TEST = 30
    EMERGENCY = 31
    TRAFFIC = 65
    SPECIAL_READING_SERVICES = 76


IQ = collections.namedtuple("IQ", ["data"])
MER = collections.namedtuple("MER", ["lower", "upper"])
BER = collections.namedtuple("BER", ["cber"])
HDC = collections.namedtuple("HDC", ["program", "data"])
Audio = collections.namedtuple("Audio", ["program", "data"])
UFID = collections.namedtuple("UFID", ["owner", "id"])
XHDR = collections.namedtuple("XHDR", ["mime", "param", "lot"])
ID3 = collections.namedtuple("ID3", ["program", "title", "artist", "album", "genre", "ufid", "xhdr"])
SIGAudioComponent = collections.namedtuple("SIGAudioComponent", ["port", "type", "mime"])
SIGDataComponent = collections.namedtuple("SIGDataComponent", ["port", "service_data_type", "type", "mime"])
SIGComponent = collections.namedtuple("SIGComponent", ["type", "id", "audio", "data"])
SIGService = collections.namedtuple("SIGService", ["type", "number", "name", "components"])
SIG = collections.namedtuple("SIG", ["services"])
LOT = collections.namedtuple("LOT", ["port", "lot", "mime", "name", "data"])
SISAudioService = collections.namedtuple("SISAudioService", ["program", "access", "type", "sound_exp"])
SISDataService = collections.namedtuple("SISDataService", ["access", "type", "mime_type"])
SIS = collections.namedtuple("SIS", ["country_code", "fcc_facility_id", "name", "slogan", "message", "alert",
                                     "latitude", "longitude", "altitude", "audio_services", "data_services"])


class _IQ(Structure):
    _fields_ = [
        ("data", POINTER(c_char)),
        ("count", c_size_t),
    ]


class _MER(Structure):
    _fields_ = [
        ("lower", c_float),
        ("upper", c_float),
    ]


class _BER(Structure):
    _fields_ = [
        ("cber", c_float),
    ]


class _HDC(Structure):
    _fields_ = [
        ("program", c_uint),
        ("data", POINTER(c_char)),
        ("count", c_size_t),
    ]


class _Audio(Structure):
    _fields_ = [
        ("program", c_uint),
        ("data", POINTER(c_char)),
        ("count", c_size_t),
    ]


class _UFID(Structure):
    _fields_ = [
        ("owner", c_char_p),
        ("id", c_char_p),
    ]


class _XHDR(Structure):
    _fields_ = [
        ("mime", c_uint32),
        ("param", c_int),
        ("lot", c_int),
    ]


class _ID3(Structure):
    _fields_ = [
        ("program", c_uint),
        ("title", c_char_p),
        ("artist", c_char_p),
        ("album", c_char_p),
        ("genre", c_char_p),
        ("ufid", _UFID),
        ("xhdr", _XHDR),
    ]


class _SIGData(Structure):
    _fields_ = [
        ("port", c_uint16),
        ("service_data_type", c_uint16),
        ("type", c_uint8),
        ("mime", c_uint32),
    ]


class _SIGAudio(Structure):
    _fields_ = [
        ("port", c_uint8),
        ("type", c_uint8),
        ("mime", c_uint32),
    ]


class _SIGUnion(Union):
    _fields_ = [
        ("audio", _SIGAudio),
        ("data", _SIGData),
    ]


class _SIGComponent(Structure):
    pass


_SIGComponent._fields_ = [
    ("next", POINTER(_SIGComponent)),
    ("type", c_uint8),
    ("id", c_uint8),
    ("u", _SIGUnion),
]


class _SIGService(Structure):
    pass


_SIGService._fields_ = [
    ("next", POINTER(_SIGService)),
    ("type", c_uint8),
    ("number", c_uint16),
    ("name", c_char_p),
    ("components", POINTER(_SIGComponent)),
]


class _SIG(Structure):
    _fields_ = [
        ("services", POINTER(_SIGService)),
    ]


class _LOT(Structure):
    _fields_ = [
        ("port", c_uint16),
        ("lot", c_uint),
        ("size", c_uint),
        ("mime", c_uint32),
        ("name", c_char_p),
        ("data", POINTER(c_char)),
    ]


class _SISAudioService(Structure):
    pass


_SISAudioService._fields_ = [
    ("next", POINTER(_SISAudioService)),
    ("program", c_uint),
    ("access", c_uint),
    ("type", c_uint),
    ("sound_exp", c_uint),
]


class _SISDataService(Structure):
    pass


_SISDataService._fields_ = [
    ("next", POINTER(_SISDataService)),
    ("access", c_uint),
    ("type", c_uint),
    ("mime_type", c_uint32),
]


class _SIS(Structure):
    _fields_ = [
        ("country_code", c_char_p),
        ("fcc_facility_id", c_int),
        ("name", c_char_p),
        ("slogan", c_char_p),
        ("message", c_char_p),
        ("alert", c_char_p),
        ("latitude", c_float),
        ("longitude", c_float),
        ("altitude", c_int),
        ("audio_services", POINTER(_SISAudioService)),
        ("data_services", POINTER(_SISDataService)),
    ]


class _EventUnion(Union):
    _fields_ = [
        ("iq", _IQ),
        ("mer", _MER),
        ("ber", _BER),
        ("hdc", _HDC),
        ("audio", _Audio),
        ("id3", _ID3),
        ("sig", _SIG),
        ("lot", _LOT),
        ("sis", _SIS),
    ]


class _Event(Structure):
    _fields_ = [
        ("event", c_uint),
        ("u", _EventUnion),
    ]


class NRSC5Error(Exception):
    pass


class NRSC5:
    libnrsc5 = None

    def _load_library(self):
        if NRSC5.libnrsc5 is None:
            if platform.system() == "Windows":
                lib_name = "libnrsc5.dll"
            elif platform.system() == "Linux":
                lib_name = "libnrsc5.so"
            elif platform.system() == "Darwin":
                lib_name = "libnrsc5.dylib"
            else:
                raise NRSC5Error("Unsupported platform: " + platform.system())
            NRSC5.libnrsc5 = cdll.LoadLibrary(lib_name)
            self.radio = c_void_p()

    def _decode(self, str):
        if str is None:
            return str
        else:
            return str.decode()

    def _callback_wrapper(self, c_evt):
        c_evt = c_evt.contents
        evt = None

        try:
            type = EventType(c_evt.event)
        except ValueError:
            return

        if type == EventType.IQ:
            iq = c_evt.u.iq
            evt = IQ(iq.data[:iq.count])
        elif type == EventType.MER:
            mer = c_evt.u.mer
            evt = MER(mer.lower, mer.upper)
        elif type == EventType.BER:
            ber = c_evt.u.ber
            evt = BER(ber.cber)
        elif type == EventType.HDC:
            hdc = c_evt.u.hdc
            evt = HDC(hdc.program, hdc.data[:hdc.count])
        elif type == EventType.AUDIO:
            audio = c_evt.u.audio
            evt = Audio(audio.program, audio.data[:audio.count * 2])
        elif type == EventType.ID3:
            id3 = c_evt.u.id3

            ufid = None
            if id3.ufid.owner or id3.ufid.id:
                ufid = UFID(self._decode(id3.ufid.owner), self._decode(id3.ufid.id))

            xhdr = None
            if id3.xhdr.mime != 0 or id3.xhdr.param != -1 or id3.xhdr.lot != -1:
                xhdr = XHDR(None if id3.xhdr.mime == 0 else MIMEType(id3.xhdr.mime),
                            None if id3.xhdr.param == -1 else id3.xhdr.param,
                            None if id3.xhdr.lot == -1 else id3.xhdr.lot)

            evt = ID3(id3.program, self._decode(id3.title), self._decode(id3.artist),
                      self._decode(id3.album), self._decode(id3.genre), ufid, xhdr)
        elif type == EventType.SIG:
            evt = []
            service_ptr = c_evt.u.sig.services
            while service_ptr:
                service = service_ptr.contents
                components = []
                component_ptr = service.components
                while component_ptr:
                    component = component_ptr.contents
                    component_type = ComponentType(component.type)
                    if component_type == ComponentType.AUDIO:
                        audio = SIGAudioComponent(component.u.audio.port, ProgramType(component.u.audio.type),
                                                  MIMEType(component.u.audio.mime))
                        components.append(SIGComponent(component_type, component.id, audio, None))
                    if component_type == ComponentType.DATA:
                        data = SIGDataComponent(component.u.data.port,
                                                ServiceDataType(component.u.data.service_data_type),
                                                component.u.data.type, MIMEType(component.u.data.mime))
                        components.append(SIGComponent(component_type, component.id, None, data))
                    component_ptr = component.next
                evt.append(SIGService(ServiceType(service.type), service.number,
                                      self._decode(service.name), components))
                service_ptr = service.next
        elif type == EventType.LOT:
            lot = c_evt.u.lot
            evt = LOT(lot.port, lot.lot, MIMEType(lot.mime), self._decode(lot.name), lot.data[:lot.size])
        elif type == EventType.SIS:
            sis = c_evt.u.sis

            latitude, longitude, altitude = None, None, None
            if not math.isnan(sis.latitude):
                latitude, longitude, altitude = sis.latitude, sis.longitude, sis.altitude

            audio_services = []
            audio_service_ptr = sis.audio_services
            while audio_service_ptr:
                asd = audio_service_ptr.contents
                audio_services.append(SISAudioService(asd.program, Access(asd.access),
                                                      ProgramType(asd.type), asd.sound_exp))
                audio_service_ptr = asd.next

            data_services = []
            data_service_ptr = sis.data_services
            while data_service_ptr:
                dsd = data_service_ptr.contents
                data_services.append(SISDataService(Access(dsd.access), ServiceDataType(dsd.type), dsd.mime_type))
                data_service_ptr = dsd.next

            evt = SIS(self._decode(sis.country_code), sis.fcc_facility_id, self._decode(sis.name),
                      self._decode(sis.slogan), self._decode(sis.message), self._decode(sis.alert),
                      latitude, longitude, altitude, audio_services, data_services)
        self.callback(type, evt)

    def __init__(self, callback):
        self._load_library()
        self.radio = c_void_p()
        self.callback = callback

    def get_version(self):
        version = c_char_p()
        NRSC5.libnrsc5.nrsc5_get_version(byref(version))
        return version.value.decode()

    def open(self, device_index, ppm_error):
        result = NRSC5.libnrsc5.nrsc5_open(byref(self.radio), device_index, ppm_error)
        if result != 0:
            raise NRSC5Error("Failed to open RTL-SDR.")
        self._set_callback()

    def open_pipe(self):
        result = NRSC5.libnrsc5.nrsc5_open_pipe(byref(self.radio))
        if result != 0:
            raise NRSC5Error("Failed to open pipe.")
        self._set_callback()

    def close(self):
        NRSC5.libnrsc5.nrsc5_close(self.radio)

    def start(self):
        NRSC5.libnrsc5.nrsc5_start(self.radio)

    def stop(self):
        NRSC5.libnrsc5.nrsc5_stop(self.radio)

    def get_frequency(self):
        frequency = c_float()
        NRSC5.libnrsc5.nrsc5_get_frequency(self.radio, byref(frequency))
        return frequency.value

    def set_frequency(self, freq):
        result = NRSC5.libnrsc5.nrsc5_set_frequency(self.radio, c_float(freq))
        if result != 0:
            raise NRSC5Error("Failed to set frequency.")

    def get_gain(self):
        gain = c_float()
        NRSC5.libnrsc5.nrsc5_get_gain(self.radio, byref(gain))
        return gain.value

    def set_gain(self, gain):
        result = NRSC5.libnrsc5.nrsc5_set_gain(self.radio, c_float(gain))
        if result != 0:
            raise NRSC5Error("Failed to set gain.")

    def set_auto_gain(self, enabled):
        NRSC5.libnrsc5.nrsc5_set_auto_gain(self.radio, int(enabled))

    def _set_callback(self):
        def callback_closure(evt, opaque):
            self._callback_wrapper(evt)

        self.callback_func = CFUNCTYPE(None, POINTER(_Event), c_void_p)(callback_closure)
        NRSC5.libnrsc5.nrsc5_set_callback(self.radio, self.callback_func, None)

    def pipe_samples_cu8(self, samples):
        if len(samples) % 4 != 0:
            raise NRSC5Error("len(samples) must be a multiple of 4.")
        result = NRSC5.libnrsc5.nrsc5_pipe_samples_cu8(self.radio, samples, len(samples))
        if result != 0:
            raise NRSC5Error("Failed to pipe samples.")

    def pipe_samples_cs16(self, samples):
        if len(samples) % 4 != 0:
            raise NRSC5Error("len(samples) must be a multiple of 4.")
        result = NRSC5.libnrsc5.nrsc5_pipe_samples_cs16(self.radio, samples, len(samples) // 2)
        if result != 0:
            raise NRSC5Error("Failed to pipe samples.")
